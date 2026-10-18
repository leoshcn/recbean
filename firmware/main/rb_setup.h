// main/rb_setup.h —— 配网表单、服务器地址与 URL 拼接（纯 C，可主机测试）。
//
// 配网页（SoftAP "RecBean-Setup"）以 application/x-www-form-urlencoded 提交：
//   ssid=<点选的网络>&ssid_manual=<手动输入>&password=<密码>&server=<host[:port]>
// 手动输入非空时覆盖点选。服务器地址统一规范化为 "host:port"（缺省端口 RB_DEFAULT_PORT=8090），
// 允许用户误填 "http://" 或 "ws://" 前缀和结尾 "/"，但不接受路径、空格和非法字符。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RB_SSID_CAP       33     // 32 字节 SSID + NUL（802.11 上限）
#define RB_PASS_CAP       65     // WPA2 口令最长 64 字节 + NUL
#define RB_SERVER_CAP     72     // "host:port" + NUL；host ≤ 63 字节
#define RB_SETUP_BODY_MAX 640    // 表单正文上限，超过直接拒绝
// 测试机宿主 8080 被占用，recbean 容器发布在 8090（容器内仍是 8080）。
#define RB_DEFAULT_SERVER "192.168.2.144:8090"
#define RB_DEFAULT_PORT   8090

typedef struct {
    char ssid[RB_SSID_CAP];
    char password[RB_PASS_CAP];
    char server[RB_SERVER_CAP];   // 已规范化的 "host:port"
} rb_setup_form_t;

// 规范化服务器地址。成功返回 true 并写入 out（"host:port"）。
// 空输入返回 false（调用方决定是否使用默认值）。
bool rb_server_normalize(const char *in, char *out, size_t out_cap);

// 解析配网表单。失败时 out 被清零（不留下半截密码）。
// server 字段为空时填 RB_DEFAULT_SERVER。
bool rb_setup_parse_form(const char *body, size_t len, rb_setup_form_t *out);

// 拼接 URL："ws://<server>/device/ws"、"http://<server><path>"。
// 缓冲不足或参数非法返回 false。
bool rb_url_ws(const char *server, char *out, size_t cap);
bool rb_url_http(const char *server, const char *path, char *out, size_t cap);

// 由 6 字节 MAC 生成设备 ID："rb-" + 12 位小写十六进制。out 至少 16 字节。
void rb_device_id_from_mac(const uint8_t mac[6], char *out, size_t cap);

// 把 src 中的 & < > " ' 转义后写入 dst（用于配网页回显 SSID/服务器地址）。
// 缓冲不足时写空串并返回 false。
bool rb_html_escape(const char *src, char *dst, size_t cap);

// Wi-Fi STA 断开原因是否属于“凭据错误”这类不值得重试的最终失败
// （与 esp_wifi_types.h 一致：15 四次握手超时、202 认证失败、204 握手超时）。
bool rb_wifi_reason_is_final(unsigned reason);
