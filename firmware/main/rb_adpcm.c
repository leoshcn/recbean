// main/rb_adpcm.c —— 标准 IMA-ADPCM 编解码与 RecBean 帧封装。格式说明见 rb_adpcm.h。
#include "rb_adpcm.h"

#include <string.h>

// 标准 IMA 步长表（89 项），来源：IMA Digital Audio Focus and Technical Working
// Groups, "Recommended Practices for Enhancing Digital Audio Compatibility in
// Multimedia Systems" rev 3.00 (1992)。服务端解码器必须使用同一张表。
static const int16_t STEP_TABLE[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

// 码字低 3 位（幅度）对应的步长索引调整量；符号位不影响调整。
static const int8_t INDEX_TABLE[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
};

// 帧头字段偏移（与 rb_adpcm.h 中的表格一致）。
#define HDR_SEQ        0
#define HDR_PREDICTOR  4
#define HDR_INDEX      6
#define HDR_RESERVED   7

static int clamp_index(int index)
{
    if (index < 0) return 0;
    if (index > 88) return 88;
    return index;
}

static int32_t clamp_sample(int32_t v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return v;
}

void rb_adpcm_reset(rb_adpcm_state_t *state)
{
    if (!state) return;
    state->predictor = 0;
    state->step_index = 0;
}

uint8_t rb_adpcm_encode_sample(rb_adpcm_state_t *state, int16_t sample)
{
    int index = clamp_index(state->step_index);
    int32_t step = STEP_TABLE[index];
    int32_t diff = (int32_t)sample - (int32_t)state->predictor;
    uint8_t code = 0;
    if (diff < 0) {
        code = 8;
        diff = -diff;
    }

    // 逐位逼近；vpdiff 的累加方式必须与解码器完全一致（step/8 + 各位贡献），
    // 否则编码端预测值与解码端重建值会逐渐漂移。
    int32_t vpdiff = step >> 3;
    if (diff >= step) {
        code |= 4;
        diff -= step;
        vpdiff += step;
    }
    step >>= 1;
    if (diff >= step) {
        code |= 2;
        diff -= step;
        vpdiff += step;
    }
    step >>= 1;
    if (diff >= step) {
        code |= 1;
        vpdiff += step;
    }

    int32_t predictor = state->predictor;
    predictor += (code & 8) ? -vpdiff : vpdiff;
    state->predictor = (int16_t)clamp_sample(predictor);
    state->step_index = (uint8_t)clamp_index(index + INDEX_TABLE[code]);
    return code;
}

int16_t rb_adpcm_decode_sample(rb_adpcm_state_t *state, uint8_t code)
{
    code &= 0x0F;
    int index = clamp_index(state->step_index);
    int32_t step = STEP_TABLE[index];
    int32_t vpdiff = step >> 3;
    if (code & 4) vpdiff += step;
    if (code & 2) vpdiff += step >> 1;
    if (code & 1) vpdiff += step >> 2;

    int32_t predictor = state->predictor;
    predictor += (code & 8) ? -vpdiff : vpdiff;
    state->predictor = (int16_t)clamp_sample(predictor);
    state->step_index = (uint8_t)clamp_index(index + INDEX_TABLE[code]);
    return state->predictor;
}

uint32_t rb_adpcm_frame_seq(const uint8_t *frame)
{
    return (uint32_t)frame[HDR_SEQ] | ((uint32_t)frame[HDR_SEQ + 1] << 8) |
           ((uint32_t)frame[HDR_SEQ + 2] << 16) | ((uint32_t)frame[HDR_SEQ + 3] << 24);
}

void rb_adpcm_set_frame_seq(uint8_t *frame, uint32_t seq)
{
    frame[HDR_SEQ] = (uint8_t)(seq & 0xFF);
    frame[HDR_SEQ + 1] = (uint8_t)((seq >> 8) & 0xFF);
    frame[HDR_SEQ + 2] = (uint8_t)((seq >> 16) & 0xFF);
    frame[HDR_SEQ + 3] = (uint8_t)((seq >> 24) & 0xFF);
}

void rb_adpcm_write_header(uint8_t *frame, uint32_t seq, const rb_adpcm_state_t *state)
{
    if (!frame || !state) return;
    // 帧头记录的是“编码第一个样本之前”的状态，解码端据此独立起步。
    rb_adpcm_set_frame_seq(frame, seq);
    uint16_t pred = (uint16_t)state->predictor;
    frame[HDR_PREDICTOR] = (uint8_t)(pred & 0xFF);
    frame[HDR_PREDICTOR + 1] = (uint8_t)(pred >> 8);
    frame[HDR_INDEX] = (uint8_t)clamp_index(state->step_index);
    frame[HDR_RESERVED] = 0;
}

void rb_adpcm_encode_into(rb_adpcm_state_t *state, const int16_t *pcm, size_t n_samples,
                          uint8_t *frame, size_t sample_offset)
{
    if (!state || !pcm || !frame) return;
    if ((n_samples | sample_offset) & 1u) return;                 // 必须成对（一个字节两个样本）
    if (sample_offset + n_samples > RB_ADPCM_FRAME_SAMPLES) return;
    uint8_t *data = frame + RB_ADPCM_HEADER_BYTES + sample_offset / 2;
    for (size_t i = 0; i < n_samples; i += 2) {
        uint8_t lo = rb_adpcm_encode_sample(state, pcm[i]);
        uint8_t hi = rb_adpcm_encode_sample(state, pcm[i + 1]);
        data[i / 2] = (uint8_t)(lo | (hi << 4));   // 低 4 位 = 较早的样本
    }
}

void rb_adpcm_encode_frame(uint32_t seq, const int16_t *pcm,
                           rb_adpcm_state_t *state, uint8_t *out)
{
    if (!pcm || !state || !out) return;
    state->step_index = (uint8_t)clamp_index(state->step_index);
    rb_adpcm_write_header(out, seq, state);
    rb_adpcm_encode_into(state, pcm, RB_ADPCM_FRAME_SAMPLES, out, 0);
}

bool rb_adpcm_decode_frame(const uint8_t *frame, size_t frame_len,
                           int16_t *pcm, uint32_t *seq)
{
    if (!frame || !pcm || frame_len != RB_ADPCM_FRAME_BYTES) return false;
    if (frame[HDR_INDEX] > 88 || frame[HDR_RESERVED] != 0) return false;

    rb_adpcm_state_t state;
    state.predictor = (int16_t)(uint16_t)(frame[HDR_PREDICTOR] |
                                          ((uint16_t)frame[HDR_PREDICTOR + 1] << 8));
    state.step_index = frame[HDR_INDEX];
    const uint8_t *data = frame + RB_ADPCM_HEADER_BYTES;
    for (size_t i = 0; i < RB_ADPCM_DATA_BYTES; i++) {
        pcm[2 * i] = rb_adpcm_decode_sample(&state, data[i] & 0x0F);
        pcm[2 * i + 1] = rb_adpcm_decode_sample(&state, (uint8_t)(data[i] >> 4));
    }
    if (seq) *seq = rb_adpcm_frame_seq(frame);
    return true;
}
