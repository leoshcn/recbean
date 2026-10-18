// main/rb_proto.c —— 协议 v1 JSON 解析/构造。约定见 rb_proto.h。
#include "rb_proto.h"

#include "rb_adpcm.h"
#include "rb_text.h"

#include <stdio.h>
#include <string.h>

// 字符串解码的临时缓冲。服务端承诺文本 ≤ 256 B（解码后），但我们不能信任对端，
// 因此按 640 B 有界解码；超出部分按 head（丢后面）/tail（丢前面）策略处理。
#define DECODE_CAP 640

typedef struct {
    const char *p;     // 当前位置
    const char *end;   // 输入末尾（不含）
} cursor_t;

typedef struct {
    char buf[DECODE_CAP];
    size_t len;
    bool keep_tail;    // true：缓冲满时丢弃开头，保留最新的字节
    bool overflowed;   // 是否丢过字节
} sink_t;

static void skip_ws(cursor_t *c)
{
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r')) {
        c->p++;
    }
}

static int hex_nibble(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static void sink_put(sink_t *s, const char *bytes, size_t n)
{
    if (!s) return;
    if (s->len + n > sizeof(s->buf)) {
        s->overflowed = true;
        if (!s->keep_tail) return;          // head 模式：后面的字节全部丢弃
        // tail 模式：丢掉前一半，并对齐到 UTF-8 字符起点，避免留下半个字符。
        size_t drop = sizeof(s->buf) / 2;
        while (drop < s->len && ((uint8_t)s->buf[drop] & 0xC0) == 0x80) drop++;
        memmove(s->buf, s->buf + drop, s->len - drop);
        s->len -= drop;
        if (s->len + n > sizeof(s->buf)) return;
    }
    memcpy(s->buf + s->len, bytes, n);
    s->len += n;
}

static bool read_hex4(cursor_t *c, uint32_t *out)
{
    if (c->end - c->p < 4) return false;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        int n = hex_nibble(c->p[i]);
        if (n < 0) return false;
        v = (v << 4) | (uint32_t)n;
    }
    c->p += 4;
    *out = v;
    return true;
}

// 解析 JSON 字符串（*c->p 必须是 '"'）。sink 为 NULL 时只跳过。
static bool parse_string(cursor_t *c, sink_t *sink)
{
    if (c->p >= c->end || *c->p != '"') return false;
    c->p++;
    while (c->p < c->end) {
        char ch = *c->p++;
        if (ch == '"') return true;
        if ((uint8_t)ch < 0x20) return false;          // JSON 不允许裸控制字符
        if (ch != '\\') {
            sink_put(sink, &ch, 1);                    // 原始字节；UTF-8 合法性稍后统一清洗
            continue;
        }
        if (c->p >= c->end) return false;
        char esc = *c->p++;
        char one;
        switch (esc) {
        case '"': one = '"'; break;
        case '\\': one = '\\'; break;
        case '/': one = '/'; break;
        case 'b': one = '\b'; break;
        case 'f': one = '\f'; break;
        case 'n': one = '\n'; break;
        case 'r': one = '\r'; break;
        case 't': one = '\t'; break;
        case 'u': {
            uint32_t cp;
            if (!read_hex4(c, &cp)) return false;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                // 高代理：必须紧跟 \uDC00..DFFF 低代理，组合成增补平面码点。
                uint32_t lo;
                if (c->end - c->p >= 6 && c->p[0] == '\\' && c->p[1] == 'u') {
                    cursor_t probe = {c->p + 2, c->end};
                    if (read_hex4(&probe, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        c->p = probe.p;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else {
                        cp = '?';
                    }
                } else {
                    cp = '?';
                }
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                cp = '?';                               // 孤立低代理
            }
            if (cp == 0) cp = '?';                      // 不允许把 NUL 写进 C 字符串
            char utf8[4];
            int n = rb_utf8_encode(cp, utf8);
            if (n > 0) sink_put(sink, utf8, (size_t)n);
            continue;
        }
        default:
            return false;
        }
        sink_put(sink, &one, 1);
    }
    return false;   // 没有闭合引号
}

static bool parse_int(cursor_t *c, int64_t *out)
{
    bool neg = false;
    if (c->p < c->end && *c->p == '-') {
        neg = true;
        c->p++;
    }
    if (c->p >= c->end || *c->p < '0' || *c->p > '9') return false;
    int64_t v = 0;
    while (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
        if (v < (INT64_MAX - 9) / 10) v = v * 10 + (*c->p - '0');   // 溢出时饱和
        c->p++;
    }
    // 小数/指数部分：协议里不会出现，出现时截断为整数部分。
    if (c->p < c->end && *c->p == '.') {
        c->p++;
        while (c->p < c->end && *c->p >= '0' && *c->p <= '9') c->p++;
    }
    if (c->p < c->end && (*c->p == 'e' || *c->p == 'E')) {
        c->p++;
        if (c->p < c->end && (*c->p == '+' || *c->p == '-')) c->p++;
        while (c->p < c->end && *c->p >= '0' && *c->p <= '9') c->p++;
    }
    *out = neg ? -v : v;
    return true;
}

static bool match_literal(cursor_t *c, const char *lit)
{
    size_t n = strlen(lit);
    if ((size_t)(c->end - c->p) < n || memcmp(c->p, lit, n) != 0) return false;
    c->p += n;
    return true;
}

// 跳过任意 JSON 值（含嵌套对象/数组）。depth 防止恶意深嵌套耗尽栈。
static bool skip_value(cursor_t *c, int depth)
{
    if (depth > 16) return false;
    skip_ws(c);
    if (c->p >= c->end) return false;
    char ch = *c->p;
    if (ch == '"') return parse_string(c, NULL);
    if (ch == '{' || ch == '[') {
        char close = ch == '{' ? '}' : ']';
        c->p++;
        skip_ws(c);
        if (c->p < c->end && *c->p == close) {
            c->p++;
            return true;
        }
        for (;;) {
            if (ch == '{') {
                skip_ws(c);
                if (!parse_string(c, NULL)) return false;
                skip_ws(c);
                if (c->p >= c->end || *c->p != ':') return false;
                c->p++;
            }
            if (!skip_value(c, depth + 1)) return false;
            skip_ws(c);
            if (c->p >= c->end) return false;
            if (*c->p == ',') {
                c->p++;
                continue;
            }
            if (*c->p == close) {
                c->p++;
                return true;
            }
            return false;
        }
    }
    if (ch == '-' || (ch >= '0' && ch <= '9')) {
        int64_t ignored;
        return parse_int(c, &ignored);
    }
    return match_literal(c, "true") || match_literal(c, "false") || match_literal(c, "null");
}

// 顶层对象遍历回调：key 为已解码键名；回调负责消费值（或返回 false 交给通用跳过）。
typedef bool (*field_cb_t)(const char *key, cursor_t *c, void *ctx, bool *consumed);

static bool walk_object(const char *json, size_t len, field_cb_t cb, void *ctx)
{
    if (!json) return false;
    cursor_t c = {json, json + len};
    skip_ws(&c);
    if (c.p >= c.end || *c.p != '{') return false;
    c.p++;
    skip_ws(&c);
    if (c.p < c.end && *c.p == '}') return true;
    for (;;) {
        skip_ws(&c);
        sink_t key = {.len = 0, .keep_tail = false, .overflowed = false};
        if (!parse_string(&c, &key)) return false;
        char name[32];
        size_t n = key.len < sizeof(name) - 1 ? key.len : sizeof(name) - 1;
        memcpy(name, key.buf, n);
        name[n] = '\0';
        skip_ws(&c);
        if (c.p >= c.end || *c.p != ':') return false;
        c.p++;
        skip_ws(&c);
        if (c.p >= c.end) return false;   // 回调可以放心读取 *c->p
        bool consumed = false;
        if (!cb(name, &c, ctx, &consumed)) return false;
        if (!consumed && !skip_value(&c, 0)) return false;
        skip_ws(&c);
        if (c.p >= c.end) return false;
        if (*c.p == ',') {
            c.p++;
            continue;
        }
        if (*c.p == '}') return true;
        return false;
    }
}

// ---------------------------------------------------------------------------
// 服务端事件
// ---------------------------------------------------------------------------

typedef struct {
    rb_msg_t *msg;
    bool has_type;
    char type[16];
    // 文本在知道 type 之前就可能出现（键顺序不保证），所以先把原始解码结果
    // 同时按 head 与 tail 两种策略各存一份，最后按 type 选择。
    sink_t text_head;
    sink_t text_tail;
    bool has_text;
    sink_t msg_text;   // error.msg
    bool has_msg;
} event_ctx_t;

static bool take_string(cursor_t *c, char *dst, size_t cap, bool *ok)
{
    sink_t s = {.len = 0, .keep_tail = false, .overflowed = false};
    if (*c->p != '"') {
        *ok = false;
        return skip_value(c, 0);
    }
    if (!parse_string(c, &s)) return false;
    rb_utf8_copy_head(dst, cap, s.buf, s.len);
    *ok = true;
    return true;
}

static bool take_int(cursor_t *c, int64_t *out, bool *ok)
{
    if (c->p < c->end && (*c->p == '-' || (*c->p >= '0' && *c->p <= '9'))) {
        *ok = parse_int(c, out);
        return *ok;
    }
    *ok = false;
    return skip_value(c, 0);   // 类型不对（例如 null）：当作缺省
}

static bool event_field(const char *key, cursor_t *c, void *vctx, bool *consumed)
{
    event_ctx_t *ctx = (event_ctx_t *)vctx;
    rb_msg_t *m = ctx->msg;
    bool ok = false;
    int64_t v = 0;
    if (strcmp(key, "type") == 0) {
        *consumed = true;
        if (!take_string(c, ctx->type, sizeof(ctx->type), &ok)) return false;
        ctx->has_type = ok;
        return true;
    }
    if (strcmp(key, "text") == 0) {
        *consumed = true;
        if (*c->p != '"') return skip_value(c, 0);
        cursor_t again = *c;
        if (!parse_string(c, &ctx->text_head)) return false;
        if (!parse_string(&again, &ctx->text_tail)) return false;
        ctx->has_text = true;
        return true;
    }
    if (strcmp(key, "msg") == 0) {
        *consumed = true;
        if (*c->p != '"') return skip_value(c, 0);
        if (!parse_string(c, &ctx->msg_text)) return false;
        ctx->has_msg = true;
        return true;
    }
    if (strcmp(key, "recording_id") == 0) {
        *consumed = true;
        if (*c->p == '"') return take_string(c, m->recording_id, sizeof(m->recording_id), &ok);
        // 兼容整数 id：格式化为十进制字符串，resume 时原样回传。
        if (!take_int(c, &v, &ok)) return false;
        if (ok) snprintf(m->recording_id, sizeof(m->recording_id), "%lld", (long long)v);
        return true;
    }
    if (strcmp(key, "code") == 0) {
        *consumed = true;
        return take_string(c, m->err_code, sizeof(m->err_code), &ok);
    }
    if (strcmp(key, "resumed") == 0) {
        *consumed = true;
        if (match_literal(c, "true")) {
            m->resumed = true;
            return true;
        }
        return skip_value(c, 0);
    }
    if (strcmp(key, "next_seq") == 0) {
        *consumed = true;
        if (!take_int(c, &v, &ok)) return false;
        if (ok && v >= 0 && v <= (int64_t)UINT32_MAX) {
            m->next_seq = v;
            m->has_next_seq = true;
        }
        return true;
    }
    struct {
        const char *name;
        int kind;   // 0: int32 字段, 1: int64 字段
        void *dst;
    } ints[] = {
        {"idx", 0, &m->idx},
        {"speaker", 0, &m->speaker},
        {"sentences", 0, &m->sentences},
        {"start_ms", 1, &m->start_ms},
        {"end_ms", 1, &m->end_ms},
        {"duration_ms", 1, &m->duration_ms},
    };
    for (size_t i = 0; i < sizeof(ints) / sizeof(ints[0]); i++) {
        if (strcmp(key, ints[i].name) != 0) continue;
        *consumed = true;
        if (!take_int(c, &v, &ok)) return false;
        if (ok) {
            if (ints[i].kind == 0) {
                if (v > INT32_MAX) v = INT32_MAX;
                if (v < INT32_MIN) v = INT32_MIN;
                *(int32_t *)ints[i].dst = (int32_t)v;
            } else {
                *(int64_t *)ints[i].dst = v;
            }
        }
        return true;
    }
    return true;   // 未知键：交给通用跳过
}

bool rb_proto_parse(const char *json, size_t len, rb_msg_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    // event_ctx_t 约 3 KiB，放 static 避免压爆 websocket 事件任务的栈。
    // 只有 websocket 事件任务调用本函数，因此不需要重入。
    static event_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.msg = out;
    ctx.text_tail.keep_tail = true;
    if (!walk_object(json, len, event_field, &ctx) || !ctx.has_type) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    const char *t = ctx.type;
    if (strcmp(t, "started") == 0) out->type = RB_MSG_STARTED;
    else if (strcmp(t, "partial") == 0) out->type = RB_MSG_PARTIAL;
    else if (strcmp(t, "final") == 0) out->type = RB_MSG_FINAL;
    else if (strcmp(t, "error") == 0) out->type = RB_MSG_ERROR;
    else if (strcmp(t, "saved") == 0) out->type = RB_MSG_SAVED;
    else out->type = RB_MSG_UNKNOWN;

    if (out->type == RB_MSG_PARTIAL && ctx.has_text) {
        out->text_len = rb_utf8_copy_tail(out->text, sizeof(out->text),
                                          ctx.text_tail.buf, ctx.text_tail.len);
        out->text_truncated = ctx.text_tail.overflowed || out->text_len < ctx.text_tail.len;
    } else if (out->type == RB_MSG_ERROR && ctx.has_msg) {
        out->text_len = rb_utf8_copy_head(out->text, sizeof(out->text),
                                          ctx.msg_text.buf, ctx.msg_text.len);
        out->text_truncated = ctx.msg_text.overflowed || out->text_len < ctx.msg_text.len;
    } else if (ctx.has_text) {
        out->text_len = rb_utf8_copy_head(out->text, sizeof(out->text),
                                          ctx.text_head.buf, ctx.text_head.len);
        out->text_truncated = ctx.text_head.overflowed || out->text_len < ctx.text_head.len;
    }
    if (out->type == RB_MSG_ERROR) {
        if (strcmp(out->err_code, "asr_error") == 0) out->err = RB_ERR_ASR;
        else if (strcmp(out->err_code, "auth") == 0) out->err = RB_ERR_AUTH;
        else if (strcmp(out->err_code, "busy") == 0) out->err = RB_ERR_BUSY;
        else out->err = RB_ERR_OTHER;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 构造设备→服务消息
// ---------------------------------------------------------------------------

// 把 src 作为 JSON 字符串内容写入（转义 " \ 和控制字符）。返回写入长度或 -1。
static int put_json_string(char *buf, size_t cap, const char *src)
{
    size_t used = 0;
    for (const uint8_t *p = (const uint8_t *)src; *p; p++) {
        char tmp[8];
        size_t n;
        if (*p == '"' || *p == '\\') {
            tmp[0] = '\\';
            tmp[1] = (char)*p;
            n = 2;
        } else if (*p < 0x20) {
            n = (size_t)snprintf(tmp, sizeof(tmp), "\\u%04x", *p);
        } else {
            tmp[0] = (char)*p;
            n = 1;
        }
        if (used + n >= cap) return -1;
        memcpy(buf + used, tmp, n);
        used += n;
    }
    buf[used] = '\0';
    return (int)used;
}

static int finish(int n, size_t cap)
{
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

int rb_proto_build_start(char *buf, size_t cap, const char *fw_version, const char *mode)
{
    if (!buf || cap == 0 || !fw_version || !mode) return -1;
    if (strcmp(mode, RB_PROTO_MODE_NOTE) != 0 && strcmp(mode, RB_PROTO_MODE_MEETING) != 0) return -1;
    char fw[40];
    if (put_json_string(fw, sizeof(fw), fw_version) < 0) return -1;
    int n = snprintf(buf, cap,
                     "{\"type\":\"start\",\"sample_rate\":%d,\"ch\":1,"
                     "\"codec\":\"ima_adpcm\",\"fw\":\"%s\",\"mode\":\"%s\"}",
                     RB_ADPCM_SAMPLE_RATE, fw, mode);
    return finish(n, cap);
}

int rb_proto_build_stop(char *buf, size_t cap)
{
    if (!buf || cap == 0) return -1;
    return finish(snprintf(buf, cap, "{\"type\":\"stop\"}"), cap);
}

int rb_proto_build_resume(char *buf, size_t cap, const char *recording_id, uint32_t next_seq)
{
    if (!buf || cap == 0 || !recording_id || !recording_id[0]) return -1;
    char id[2 * RB_PROTO_ID_MAX + 8];
    if (put_json_string(id, sizeof(id), recording_id) < 0) return -1;
    int n = snprintf(buf, cap, "{\"type\":\"resume\",\"recording_id\":\"%s\",\"next_seq\":%lu}",
                     id, (unsigned long)next_seq);
    return finish(n, cap);
}

int rb_proto_build_pair_request(char *buf, size_t cap, const char *device_id)
{
    if (!buf || cap == 0 || !device_id || !device_id[0]) return -1;
    char id[80];
    if (put_json_string(id, sizeof(id), device_id) < 0) return -1;
    return finish(snprintf(buf, cap, "{\"device_id\":\"%s\"}", id), cap);
}

// ---------------------------------------------------------------------------
// 配对回复
// ---------------------------------------------------------------------------

static bool token_ok(const char *t)
{
    size_t n = strlen(t);
    if (n == 0 || n > RB_PROTO_TOKEN_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        if ((uint8_t)t[i] <= 0x20 || (uint8_t)t[i] >= 0x7F) return false;
    }
    return true;
}

static bool pair_field(const char *key, cursor_t *c, void *vctx, bool *consumed)
{
    rb_pair_reply_t *r = (rb_pair_reply_t *)vctx;
    bool ok = false;
    if (strcmp(key, "code") == 0) {
        *consumed = true;
        char tmp[16];
        if (*c->p == '"') {
            if (!take_string(c, tmp, sizeof(tmp), &ok)) return false;
        } else {
            int64_t v = 0;
            if (!take_int(c, &v, &ok)) return false;
            if (ok && v >= 0 && v <= 9999) snprintf(tmp, sizeof(tmp), "%04d", (int)v);
            else ok = false;
        }
        if (ok && strlen(tmp) == RB_PROTO_PAIR_CODE) {
            bool digits = true;
            for (int i = 0; i < RB_PROTO_PAIR_CODE; i++) {
                if (tmp[i] < '0' || tmp[i] > '9') digits = false;
            }
            if (digits) {
                memcpy(r->code, tmp, RB_PROTO_PAIR_CODE + 1);
                r->has_code = true;
            }
        }
        return true;
    }
    if (strcmp(key, "token") == 0) {
        *consumed = true;
        char tmp[RB_PROTO_TOKEN_MAX + 2];
        if (!take_string(c, tmp, sizeof(tmp), &ok)) return false;
        if (ok && token_ok(tmp)) {
            memcpy(r->token, tmp, strlen(tmp) + 1);
            r->has_token = true;
        }
        return true;
    }
    if (strcmp(key, "status") == 0) {
        *consumed = true;
        return take_string(c, r->status, sizeof(r->status), &ok);
    }
    return true;
}

bool rb_proto_parse_pair(const char *json, size_t len, rb_pair_reply_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!walk_object(json, len, pair_field, out)) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    return true;
}
