// tests/test_rb_adpcm.c —— IMA-ADPCM 编码器/解码器主机测试。
//
// 1. 与服务端共用的测试向量（tests/fixtures/adpcm_vectors.json，复制自
//    server/tests/fixtures/adpcm_vectors.json，validate.sh 会校验两份一致）：
//    编码结果、帧字节、帧末状态、解码结果必须逐字节一致。
// 2. 自洽性：连续编码多帧，每帧都能凭帧头独立解码，且与连续解码结果一致。
// 用法：test_rb_adpcm <adpcm_vectors.json>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rb_adpcm.h"

#define TEST_PI 3.14159265358979323846   // -std=c11 下 math.h 不保证提供 TEST_PI

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    assert(f && "cannot open vectors file");
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    assert(buf);
    assert(fread(buf, 1, (size_t)n, f) == (size_t)n);
    buf[n] = '\0';
    fclose(f);
    *len = (size_t)n;
    return buf;
}

// 在 [obj, end) 中找 "key": 后面的位置。
static const char *find_key(const char *obj, const char *end, const char *key)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = obj;
    size_t n = strlen(pat);
    while ((p = strstr(p, pat)) != NULL && p < end) {
        const char *q = p + n;
        while (*q == ' ') q++;
        if (*q == ':') {
            q++;
            while (*q == ' ') q++;
            return q;
        }
        p += n;
    }
    return NULL;
}

static long get_int(const char *obj, const char *end, const char *key, int *found)
{
    const char *v = find_key(obj, end, key);
    *found = v != NULL;
    return v ? strtol(v, NULL, 10) : 0;
}

static size_t get_int_array(const char *obj, const char *end, const char *key,
                            int16_t *out, size_t cap)
{
    const char *v = find_key(obj, end, key);
    assert(v && *v == '[');
    v++;
    size_t n = 0;
    while (*v && *v != ']') {
        char *next;
        long x = strtol(v, &next, 10);
        assert(next != v);
        assert(n < cap);
        out[n++] = (int16_t)x;
        v = next;
        while (*v == ',' || *v == ' ') v++;
    }
    return n;
}

static size_t get_hex(const char *obj, const char *end, const char *key, uint8_t *out, size_t cap)
{
    const char *v = find_key(obj, end, key);
    if (!v) return 0;
    assert(*v == '"');
    v++;
    size_t n = 0;
    while (v[0] && v[0] != '"') {
        unsigned byte;
        assert(sscanf(v, "%2x", &byte) == 1);
        assert(n < cap);
        out[n++] = (uint8_t)byte;
        v += 2;
    }
    return n;
}

static void check_vectors(const char *path)
{
    size_t len;
    char *json = read_file(path, &len);
    const char *end = json + len;
    int checked = 0;
    const char *p = strstr(json, "\"vectors\"");
    assert(p);
    static int16_t pcm[RB_ADPCM_FRAME_SAMPLES];
    static int16_t decoded[RB_ADPCM_FRAME_SAMPLES];
    static int16_t ours[RB_ADPCM_FRAME_SAMPLES];
    static uint8_t adpcm[RB_ADPCM_DATA_BYTES];
    static uint8_t frame[RB_ADPCM_FRAME_BYTES];
    static uint8_t built[RB_ADPCM_FRAME_BYTES];
    while ((p = strstr(p, "\"name\"")) != NULL) {
        const char *next = strstr(p + 6, "\"name\"");
        const char *obj_end = next ? next : end;
        int found;
        long predictor = get_int(p, obj_end, "predictor", &found);
        assert(found);
        long step_index = get_int(p, obj_end, "step_index", &found);
        assert(found);
        long end_predictor = get_int(p, obj_end, "end_predictor", &found);
        assert(found);
        long end_step_index = get_int(p, obj_end, "end_step_index", &found);
        assert(found);
        size_t n = get_int_array(p, obj_end, "pcm", pcm, RB_ADPCM_FRAME_SAMPLES);
        size_t nd = get_int_array(p, obj_end, "decoded", decoded, RB_ADPCM_FRAME_SAMPLES);
        size_t na = get_hex(p, obj_end, "adpcm_hex", adpcm, sizeof(adpcm));
        assert(nd == n && na == n / 2);

        // 编码：逐样本，与向量的 adpcm_hex 和帧末状态比对。
        rb_adpcm_state_t st = {(int16_t)predictor, (uint8_t)step_index};
        for (size_t i = 0; i < n; i += 2) {
            uint8_t lo = rb_adpcm_encode_sample(&st, pcm[i]);
            uint8_t hi = rb_adpcm_encode_sample(&st, pcm[i + 1]);
            assert((uint8_t)(lo | (hi << 4)) == adpcm[i / 2]);
        }
        assert(st.predictor == end_predictor && st.step_index == end_step_index);

        // 解码：与向量 decoded 比对。
        rb_adpcm_state_t ds = {(int16_t)predictor, (uint8_t)step_index};
        for (size_t i = 0; i < n; i += 2) {
            ours[i] = rb_adpcm_decode_sample(&ds, adpcm[i / 2] & 0x0F);
            ours[i + 1] = rb_adpcm_decode_sample(&ds, adpcm[i / 2] >> 4);
        }
        assert(memcmp(ours, decoded, n * sizeof(int16_t)) == 0);
        assert(ds.predictor == end_predictor && ds.step_index == end_step_index);

        // 完整帧：帧编码字节与 frame_hex 一致，帧解码与 decoded 一致。
        size_t nf = get_hex(p, obj_end, "frame_hex", frame, sizeof(frame));
        if (nf > 0) {
            assert(nf == RB_ADPCM_FRAME_BYTES && n == RB_ADPCM_FRAME_SAMPLES);
            long seq = get_int(p, obj_end, "seq", &found);
            assert(found);
            rb_adpcm_state_t fs = {(int16_t)predictor, (uint8_t)step_index};
            rb_adpcm_encode_frame((uint32_t)seq, pcm, &fs, built);
            assert(memcmp(built, frame, RB_ADPCM_FRAME_BYTES) == 0);
            assert(fs.predictor == end_predictor && fs.step_index == end_step_index);
            uint32_t got_seq = 0;
            assert(rb_adpcm_decode_frame(frame, sizeof(frame), ours, &got_seq));
            assert(got_seq == (uint32_t)seq);
            assert(memcmp(ours, decoded, sizeof(ours)) == 0);
        }
        checked++;
        p = obj_end;
        if (!next) break;
    }
    assert(checked >= 4);
    printf("test_rb_adpcm: %d shared vectors OK\n", checked);
    free(json);
}

static void check_self_consistency(void)
{
    // 合成一段带包络的“语音”：多频正弦 + 伪随机噪声，覆盖大幅度与静音。
    static int16_t pcm[5][RB_ADPCM_FRAME_SAMPLES];
    uint32_t rng = 12345;
    for (int f = 0; f < 5; f++) {
        for (int i = 0; i < RB_ADPCM_FRAME_SAMPLES; i++) {
            int t = f * RB_ADPCM_FRAME_SAMPLES + i;
            rng = rng * 1103515245u + 12345u;
            int noise = (int)((rng >> 16) & 0x1FF) - 256;
            double amp = (f == 3) ? 0.0 : 1.0;
            int v = (int)(amp * (6000.0 * sin(2.0 * TEST_PI * 220.0 * t / 16000.0) +
                                 3000.0 * sin(2.0 * TEST_PI * 570.0 * t / 16000.0))) + noise;
            if (f == 4) v = (i & 64) ? 32767 : -32768;   // 方波，测试饱和
            pcm[f][i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
        }
    }
    rb_adpcm_state_t enc;
    rb_adpcm_reset(&enc);
    rb_adpcm_state_t cont;
    rb_adpcm_reset(&cont);
    static uint8_t frame[RB_ADPCM_FRAME_BYTES];
    static int16_t out[RB_ADPCM_FRAME_SAMPLES];
    for (int f = 0; f < 5; f++) {
        rb_adpcm_encode_frame((uint32_t)(100 + f), pcm[f], &enc, frame);
        assert(frame[7] == 0);
        assert(rb_adpcm_frame_seq(frame) == (uint32_t)(100 + f));
        uint32_t seq = 0;
        assert(rb_adpcm_decode_frame(frame, sizeof(frame), out, &seq));
        assert(seq == (uint32_t)(100 + f));
        // 独立解码与连续解码完全一致（帧头状态 = 编码端当时的状态）。
        for (int i = 0; i < RB_ADPCM_FRAME_SAMPLES; i += 2) {
            uint8_t b = frame[RB_ADPCM_HEADER_BYTES + i / 2];
            assert(rb_adpcm_decode_sample(&cont, b & 0x0F) == out[i]);
            assert(rb_adpcm_decode_sample(&cont, b >> 4) == out[i + 1]);
        }
        assert(cont.predictor == enc.predictor && cont.step_index == enc.step_index);
        // 粗略音质：非方波帧的平均绝对误差应远小于信号幅度。
        if (f < 4) {
            long long err = 0;
            long long sig = 0;
            for (int i = 0; i < RB_ADPCM_FRAME_SAMPLES; i++) {
                err += llabs((long long)out[i] - pcm[f][i]);
                sig += llabs((long long)pcm[f][i]);
            }
            // 正弦叠加信号的平均绝对误差应 < 5%；只有小噪声的帧误差应很小。
            if (f == 3) assert(err / RB_ADPCM_FRAME_SAMPLES < 200);
            else assert(err * 100 < sig * 5);
        }
    }
    // 分块编码（20 ms 一块，与设备采集任务相同）与整帧编码逐字节一致。
    {
        static uint8_t whole[RB_ADPCM_FRAME_BYTES];
        static uint8_t chunked[RB_ADPCM_FRAME_BYTES];
        rb_adpcm_state_t a = {-1234, 40};
        rb_adpcm_state_t b = a;
        rb_adpcm_encode_frame(9, pcm[0], &a, whole);
        rb_adpcm_write_header(chunked, 9, &b);
        for (size_t off = 0; off < RB_ADPCM_FRAME_SAMPLES; off += 320) {
            rb_adpcm_encode_into(&b, pcm[0] + off, 320, chunked, off);
        }
        assert(memcmp(whole, chunked, sizeof(whole)) == 0);
        assert(a.predictor == b.predictor && a.step_index == b.step_index);
    }
    // 损坏帧被拒绝。
    frame[6] = 89;
    assert(!rb_adpcm_decode_frame(frame, sizeof(frame), out, NULL));
    frame[6] = 0;
    frame[7] = 1;
    assert(!rb_adpcm_decode_frame(frame, sizeof(frame), out, NULL));
    frame[7] = 0;
    assert(!rb_adpcm_decode_frame(frame, sizeof(frame) - 1, out, NULL));
    rb_adpcm_set_frame_seq(frame, 0xA1B2C3D4u);
    assert(frame[0] == 0xD4 && frame[3] == 0xA1);
    assert(rb_adpcm_frame_seq(frame) == 0xA1B2C3D4u);
}

int main(int argc, char **argv)
{
    assert(argc >= 2 && "usage: test_rb_adpcm <adpcm_vectors.json>");
    assert(RB_ADPCM_FRAME_BYTES == 1608);
    check_vectors(argv[1]);
    check_self_consistency();
    return 0;
}
