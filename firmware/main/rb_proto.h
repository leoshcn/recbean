// main/rb_proto.h —— 设备↔服务协议 v1 的 JSON 消息解析与构造（纯 C，可主机测试）。
//
// 协议定义见 .trellis/tasks/10-18-recbean-mvp/design.md §3：
//   设备 → 服务（文本帧）
//     {"type":"start","sample_rate":16000,"ch":1,"codec":"ima_adpcm","fw":"x.y.z"}
//     {"type":"stop"}
//     {"type":"resume","recording_id":"..","next_seq":N}
//   服务 → 设备（文本帧）
//     {"type":"started","recording_id":"..."}
//       续传时服务端额外带 "resumed":true,"next_seq":N（N = 服务端已收到的下一帧），
//       设备据此从 N 开始重发；没有该字段时从缓冲中最旧帧重发，服务端按 seq 去重。
//     {"type":"partial","text":"..."}
//     {"type":"final","idx":n,"speaker":1,"start_ms":..,"end_ms":..,"text":"..."}
//     {"type":"error","code":"asr_error|auth|busy","msg":"..."}
//     {"type":"saved","recording_id":"...","duration_ms":..,"sentences":n}
//   配对（HTTP）
//     POST /device/pair {"device_id":".."} → 含 4 位 "code" 的 JSON
//     GET  /device/pair/{device_id}       → 确认后含 "token" 的 JSON
//
// 解析器只处理顶层扁平对象：认识的键取值，其余键（包括嵌套对象/数组）跳过，
// 便于服务端以后加字段而不破坏旧固件。字符串完整处理 JSON 转义（含 \uXXXX
// 与代理对），解码结果再做 UTF-8 清洗与按字符边界截断。
#pragma once

#include "rb_transcript.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RB_PROTO_ID_MAX     63     // recording_id 最大字节数
#define RB_PROTO_CODE_MAX   23     // error.code 最大字节数
#define RB_PROTO_TOKEN_MAX  127    // 设备令牌最大字节数（ASCII 可见字符）
#define RB_PROTO_PAIR_CODE  4      // 配对码位数

typedef enum {
    RB_MSG_UNKNOWN = 0,   // 合法 JSON 但 type 不认识：调用方忽略即可
    RB_MSG_STARTED,
    RB_MSG_PARTIAL,
    RB_MSG_FINAL,
    RB_MSG_ERROR,
    RB_MSG_SAVED,
} rb_msg_type_t;

typedef enum {
    RB_ERR_OTHER = 0,     // 未知错误码
    RB_ERR_ASR,           // "asr_error"：识别失败，音频仍在保存
    RB_ERR_AUTH,          // "auth"：令牌无效/未配对
    RB_ERR_BUSY,          // "busy"：服务端忙（例如已有录音进行中）
} rb_err_code_t;

typedef struct {
    rb_msg_type_t type;
    char recording_id[RB_PROTO_ID_MAX + 1];   // started / saved
    char text[RB_TS_TEXT_MAX + 1];            // partial / final 的文本；error 的 msg
    size_t text_len;                          // text 的字节数
    bool text_truncated;                      // 解码后超过 RB_TS_TEXT_MAX 被截断
    int32_t idx;                              // final
    int32_t speaker;                          // final；缺省 0（未知）
    int64_t start_ms;                         // final
    int64_t end_ms;                           // final
    rb_err_code_t err;                        // error
    char err_code[RB_PROTO_CODE_MAX + 1];     // error 原始 code 字符串
    int64_t duration_ms;                      // saved
    int32_t sentences;                        // saved
    bool resumed;                             // started：服务端扩展字段 "resumed":true
    bool has_next_seq;                        // started（续传）：服务端期望的下一帧序号
    int64_t next_seq;
} rb_msg_t;

// 解析一条服务端消息。json 不要求以 NUL 结尾（长度 len）。
// 返回 false：不是合法的顶层 JSON 对象，或缺少 type 字符串。
// partial 文本保留末尾、其余文本保留开头（各 ≤ RB_TS_TEXT_MAX 字节）。
bool rb_proto_parse(const char *json, size_t len, rb_msg_t *out);

// 构造设备→服务消息。成功返回写入长度（不含 NUL），缓冲不足或参数非法返回 -1。
int rb_proto_build_start(char *buf, size_t cap, const char *fw_version);
int rb_proto_build_stop(char *buf, size_t cap);
int rb_proto_build_resume(char *buf, size_t cap, const char *recording_id, uint32_t next_seq);
int rb_proto_build_pair_request(char *buf, size_t cap, const char *device_id);

typedef struct {
    bool has_code;
    char code[RB_PROTO_PAIR_CODE + 1];        // 4 位数字配对码
    bool has_token;
    char token[RB_PROTO_TOKEN_MAX + 1];       // 非空即配对成功
    char status[24];                          // 可选的 "status" 字段，原样（ASCII）
} rb_pair_reply_t;

// 解析配对接口的回复：code 可以是字符串或整数（整数补零到 4 位）；
// token 只接受 1..RB_PROTO_TOKEN_MAX 个可见 ASCII 字符（不能放进 HTTP 头的字符被拒绝）。
bool rb_proto_parse_pair(const char *json, size_t len, rb_pair_reply_t *out);
