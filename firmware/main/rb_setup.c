// main/rb_setup.c —— 配网表单与地址处理。约定见 rb_setup.h。
#include "rb_setup.h"

#include <stdio.h>
#include <string.h>

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// URL 解码 in[0..in_len) 到 out。'+' → 空格；拒绝 %00 和不完整的 %XX。
static bool url_decode(const char *in, size_t in_len, char *out, size_t cap)
{
    if (!out || cap == 0) return false;
    size_t used = 0;
    for (size_t i = 0; i < in_len; i++) {
        unsigned char ch = (unsigned char)in[i];
        if (ch == '+') {
            ch = ' ';
        } else if (ch == '%') {
            if (i + 2 >= in_len) return false;   // 需要 in[i+1]、in[i+2] 两位十六进制
            int hi = hex_nibble(in[i + 1]);
            int lo = hex_nibble(in[i + 2]);
            if (hi < 0 || lo < 0) return false;
            ch = (unsigned char)((hi << 4) | lo);
            if (ch == 0) return false;
            i += 2;
        }
        if (used + 1 >= cap) return false;
        out[used++] = (char)ch;
    }
    out[used] = '\0';
    return true;
}

static bool host_char_ok(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '.' || c == '-';
}

bool rb_server_normalize(const char *in, char *out, size_t out_cap)
{
    if (!in || !out || out_cap == 0) return false;
    out[0] = '\0';
    const char *p = in;
    while (*p == ' ') p++;
    const char *prefixes[] = {"http://", "ws://"};
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        size_t n = strlen(prefixes[i]);
        if (strncmp(p, prefixes[i], n) == 0) {
            p += n;
            break;
        }
    }
    size_t len = strlen(p);
    while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '/')) len--;
    if (len == 0) return false;

    // 拆出 host 与可选端口；不支持 IPv6 字面量（局域网第一版用不到）。
    size_t host_len = len;
    long port = RB_DEFAULT_PORT;
    const char *colon = memchr(p, ':', len);
    if (colon) {
        host_len = (size_t)(colon - p);
        const char *q = colon + 1;
        size_t port_len = len - host_len - 1;
        if (port_len == 0 || port_len > 5) return false;
        port = 0;
        for (size_t i = 0; i < port_len; i++) {
            if (q[i] < '0' || q[i] > '9') return false;
            port = port * 10 + (q[i] - '0');
        }
        if (port < 1 || port > 65535) return false;
    }
    if (host_len == 0 || host_len > 63) return false;
    for (size_t i = 0; i < host_len; i++) {
        if (!host_char_ok(p[i])) return false;
    }
    if (p[0] == '-' || p[0] == '.' || p[host_len - 1] == '-' || p[host_len - 1] == '.') return false;
    int n = snprintf(out, out_cap, "%.*s:%ld", (int)host_len, p, port);
    if (n < 0 || (size_t)n >= out_cap) {
        out[0] = '\0';
        return false;
    }
    return true;
}

bool rb_setup_parse_form(const char *body, size_t len, rb_setup_form_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!body || len == 0 || len > RB_SETUP_BODY_MAX) return false;

    char manual[RB_SSID_CAP] = {0};
    char server_raw[RB_SERVER_CAP + 16] = {0};
    bool ok = true;
    size_t i = 0;
    while (i < len && ok) {
        size_t start = i;
        while (i < len && body[i] != '&') i++;
        size_t part_len = i - start;
        const char *part = body + start;
        const char *eq = memchr(part, '=', part_len);
        if (eq) {
            char key[16];
            size_t key_len = (size_t)(eq - part);
            size_t val_len = part_len - key_len - 1;
            if (url_decode(part, key_len, key, sizeof(key))) {
                char *dest = NULL;
                size_t cap = 0;
                if (strcmp(key, "ssid") == 0) {
                    dest = out->ssid;
                    cap = sizeof(out->ssid);
                } else if (strcmp(key, "ssid_manual") == 0) {
                    dest = manual;
                    cap = sizeof(manual);
                } else if (strcmp(key, "password") == 0) {
                    dest = out->password;
                    cap = sizeof(out->password);
                } else if (strcmp(key, "server") == 0) {
                    dest = server_raw;
                    cap = sizeof(server_raw);
                }
                if (dest && !url_decode(eq + 1, val_len, dest, cap)) ok = false;
            }
        }
        if (i < len) i++;   // 跳过 '&'
    }
    if (ok && manual[0] != '\0') memcpy(out->ssid, manual, sizeof(out->ssid));
    if (ok && out->ssid[0] == '\0') ok = false;
    // WPA2 口令至少 8 字节，长度 1..7 必然连不上，直接拒绝；开放网络留空。
    size_t pass_len = strlen(out->password);
    if (ok && pass_len > 0 && pass_len < 8) ok = false;
    if (ok) {
        bool blank = true;
        for (const char *p = server_raw; *p; p++) {
            if (*p != ' ') blank = false;
        }
        if (blank) {
            snprintf(out->server, sizeof(out->server), "%s", RB_DEFAULT_SERVER);
        } else if (!rb_server_normalize(server_raw, out->server, sizeof(out->server))) {
            ok = false;
        }
    }
    memset(manual, 0, sizeof(manual));
    if (!ok) memset(out, 0, sizeof(*out));
    return ok;
}

bool rb_url_ws(const char *server, char *out, size_t cap)
{
    if (!server || !server[0] || !out || cap == 0) return false;
    int n = snprintf(out, cap, "ws://%s/device/ws", server);
    return n > 0 && (size_t)n < cap;
}

bool rb_url_http(const char *server, const char *path, char *out, size_t cap)
{
    if (!server || !server[0] || !path || path[0] != '/' || !out || cap == 0) return false;
    int n = snprintf(out, cap, "http://%s%s", server, path);
    return n > 0 && (size_t)n < cap;
}

void rb_device_id_from_mac(const uint8_t mac[6], char *out, size_t cap)
{
    if (!out || cap == 0) return;
    if (!mac) {
        out[0] = '\0';
        return;
    }
    snprintf(out, cap, "rb-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

bool rb_html_escape(const char *src, char *dst, size_t cap)
{
    if (!dst || cap == 0) return false;
    dst[0] = '\0';
    if (!src) return true;
    size_t used = 0;
    for (const char *p = src; *p; p++) {
        const char *rep = NULL;
        char one[2] = {*p, '\0'};
        switch (*p) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        case '\'': rep = "&#39;"; break;
        default: rep = one; break;
        }
        size_t n = strlen(rep);
        if (used + n + 1 > cap) {
            dst[0] = '\0';
            return false;
        }
        memcpy(dst + used, rep, n);
        used += n;
    }
    dst[used] = '\0';
    return true;
}

bool rb_wifi_reason_is_final(unsigned reason)
{
    return reason == 15 || reason == 202 || reason == 204;
}
