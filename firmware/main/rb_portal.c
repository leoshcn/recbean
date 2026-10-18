// main/rb_portal.c —— SoftAP 配网门户实现（通配 DNS + 配置页）。约定见 rb_portal.h。
//
// DNS 应答与 HTTP 表单接收参考了 ai-passport feature/xhs-dashboard 分支 main/xhs_net.c
// 的做法（有界解析、超时、失败不留残余），页面与流程为 RecBean 重写。
#include "rb_portal.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "rb_portal";

static httpd_handle_t s_httpd;
static TaskHandle_t s_dns_task;
static volatile bool s_dns_stop;
static TaskHandle_t s_notify;

// 以下共享数据由 s_mux 保护（httpd 任务与 rb_net 任务都会访问）。
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static rb_scan_ap_t s_scan[RB_PORTAL_SCAN_MAX];
static size_t s_scan_count;
static rb_portal_state_t s_state;
static bool s_form_ready;
static rb_setup_form_t s_form;
static bool s_rescan;
static char s_prefill[RB_SERVER_CAP];

// AP 默认地址 192.168.4.1（ESP-IDF 默认 SoftAP 网段）。
#define AP_IP_BE 0xC0A80401u

// ---------------------------------------------------------------------------
// 通配 DNS：所有 A 记录查询都回答 AP 地址，让手机弹出/打开配置页。
// ---------------------------------------------------------------------------

static void dns_reply(int sock, const uint8_t *req, int len, const struct sockaddr_in *from)
{
    if (len < 12 || len > 256) return;
    if (((req[4] << 8) | req[5]) != 1) return;          // 只处理单问题查询
    int end = 12;
    while (end < len && req[end] != 0) {
        int label = req[end];
        if ((label & 0xC0) != 0 || end + 1 + label >= len) return;
        end += 1 + label;
    }
    if (end >= len) return;
    end += 5;                                            // 0 结束符 + QTYPE + QCLASS
    if (end > len || end + 16 > 300) return;
    uint8_t resp[300];
    memcpy(resp, req, (size_t)end);
    resp[2] = 0x81;                                      // 标准响应，递归可用
    resp[3] = 0x80;
    resp[6] = 0;                                         // ANCOUNT = 1
    resp[7] = 1;
    resp[8] = resp[9] = resp[10] = resp[11] = 0;
    int at = end;
    const uint8_t answer[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4};   // 指针 + A/IN + TTL 30 s
    memcpy(resp + at, answer, sizeof(answer));
    at += (int)sizeof(answer);
    uint32_t ip = htonl(AP_IP_BE);
    memcpy(resp + at, &ip, 4);
    at += 4;
    (void)sendto(sock, resp, (size_t)at, 0, (const struct sockaddr *)from, sizeof(*from));
}

static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock >= 0) {
        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(53);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            close(sock);
            sock = -1;
        }
    }
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket 失败");
    } else {
        struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
        (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        while (!s_dns_stop) {
            uint8_t buf[256];
            struct sockaddr_in from;
            socklen_t from_len = sizeof(from);
            int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
            if (n > 0) dns_reply(sock, buf, n, &from);
        }
        close(sock);
    }
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// HTTP 页面
// ---------------------------------------------------------------------------

static const char PAGE_HEAD[] =
    "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
static const char PAGE_STYLE[] =
    "<title>RecBean-Setup</title><style>"
    "*{box-sizing:border-box}body{margin:0;background:#12161c;color:#e8edf3;"
    "font-family:system-ui,\"PingFang SC\",\"Microsoft YaHei\",sans-serif;line-height:1.5}"
    "main{max-width:28rem;margin:0 auto;padding:1.2rem}"
    "h1{font-size:1.4rem;margin:.2rem 0 .3rem}.sub{color:#9aa6b2;font-size:.9rem;margin:0 0 1rem}"
    ".ap{display:flex;gap:.6rem;align-items:center;min-height:44px;border-bottom:1px solid #2a323c}"
    ".ap span{flex:1;word-break:break-all}.ap small{color:#9aa6b2}"
    "label{display:block;margin:.9rem 0 .2rem;color:#9aa6b2;font-size:.85rem}"
    "input[type=text],input[type=password]{width:100%;min-height:44px;padding:.4rem .7rem;"
    "border:1px solid #2a323c;border-radius:10px;background:#1b2129;color:#e8edf3;font:inherit}"
    "button{width:100%;min-height:46px;margin-top:1.2rem;border:0;border-radius:999px;"
    "background:#e53950;color:#fff;font:inherit;font-weight:600}"
    "a{color:#ff8a9a}.err{color:#ff8a9a}.ok{color:#7ddc9a}"
    "</style></head><body><main>";
static const char PAGE_TAIL[] = "</main></body></html>";

static esp_err_t chunk(httpd_req_t *req, const char *s)
{
    return httpd_resp_send_chunk(req, s, (ssize_t)strlen(s));
}

static esp_err_t send_page(httpd_req_t *req, const char *refresh_url, const char *body)
{
    esp_err_t err = httpd_resp_set_type(req, "text/html; charset=utf-8");
    if (err == ESP_OK) err = httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (err == ESP_OK) err = chunk(req, PAGE_HEAD);
    if (err == ESP_OK && refresh_url) {
        char meta[96];
        snprintf(meta, sizeof(meta), "<meta http-equiv=\"refresh\" content=\"2;url=%s\">", refresh_url);
        err = chunk(req, meta);
    }
    if (err == ESP_OK) err = chunk(req, PAGE_STYLE);
    if (err == ESP_OK && body) err = chunk(req, body);
    return err;
}

static esp_err_t finish_page(httpd_req_t *req, esp_err_t err)
{
    if (err == ESP_OK) err = chunk(req, PAGE_TAIL);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    return err;
}

static esp_err_t send_setup_page(httpd_req_t *req)
{
    rb_scan_ap_t list[RB_PORTAL_SCAN_MAX];
    size_t count;
    rb_portal_state_t state;
    char prefill[RB_SERVER_CAP];
    taskENTER_CRITICAL(&s_mux);
    count = s_scan_count;
    memcpy(list, s_scan, sizeof(list));
    state = s_state;
    memcpy(prefill, s_prefill, sizeof(prefill));
    taskEXIT_CRITICAL(&s_mux);

    esp_err_t err = send_page(req, NULL,
                              "<h1>录音豆 · 配网</h1>"
                              "<p class=\"sub\">选择 2.4 GHz Wi-Fi，填写密码和录音豆服务器地址。</p>");
    if (err == ESP_OK && state == RB_PORTAL_FAILED) {
        err = chunk(req, "<p class=\"err\">上次没有连上，请检查名称和密码后重试。</p>");
    }
    if (err == ESP_OK) err = chunk(req, "<form method=\"post\" action=\"/setup\">");
    if (err == ESP_OK && count == 0) {
        err = chunk(req, "<p class=\"sub\">没有扫描到网络，请在下面手动输入名称。</p>");
    }
    for (size_t i = 0; err == ESP_OK && i < count; i++) {
        char name[6 * RB_SSID_CAP];
        if (!rb_html_escape(list[i].ssid, name, sizeof(name))) continue;
        char row[sizeof(name) * 2 + 200];
        int n = snprintf(row, sizeof(row),
                         "<label class=\"ap\"><input type=\"radio\" name=\"ssid\" value=\"%s\">"
                         "<span>%s</span><small>%s%d dBm</small></label>",
                         name, name, list[i].open ? "开放 · " : "", (int)list[i].rssi);
        if (n > 0 && (size_t)n < sizeof(row)) err = chunk(req, row);
    }
    char server_html[6 * RB_SERVER_CAP];
    if (!rb_html_escape(prefill, server_html, sizeof(server_html))) server_html[0] = '\0';
    char tail[sizeof(server_html) + 900];
    snprintf(tail, sizeof(tail),
             "<label>或手动输入网络名称</label>"
             "<input type=\"text\" name=\"ssid_manual\" maxlength=\"32\" autocomplete=\"off\">"
             "<label>Wi-Fi 密码（开放网络留空）</label>"
             "<input type=\"password\" name=\"password\" maxlength=\"64\" autocomplete=\"off\">"
             "<label>服务器地址（IP:端口）</label>"
             "<input type=\"text\" name=\"server\" maxlength=\"70\" value=\"%s\" autocomplete=\"off\">"
             "<button type=\"submit\">连接</button></form>"
             "<p><a href=\"/rescan\">重新扫描网络</a></p>",
             server_html);
    if (err == ESP_OK) err = chunk(req, tail);
    return finish_page(req, err);
}

static esp_err_t send_status_page(httpd_req_t *req)
{
    rb_portal_state_t state;
    taskENTER_CRITICAL(&s_mux);
    state = s_state;
    taskEXIT_CRITICAL(&s_mux);
    switch (state) {
    case RB_PORTAL_JOINED:
        return finish_page(req, send_page(req, NULL,
            "<h1 class=\"ok\">已连上 Wi-Fi</h1>"
            "<p>请断开本热点，然后在录音豆网页的“设备配对”中输入设备屏幕上显示的 4 位配对码。</p>"));
    case RB_PORTAL_FAILED:
        return finish_page(req, send_page(req, NULL,
            "<h1 class=\"err\">没有连上</h1>"
            "<p>请检查 Wi-Fi 名称和密码。错误的密码不会被保存。</p><p><a href=\"/\">返回重试</a></p>"));
    default:
        return finish_page(req, send_page(req, "/status",
            "<h1>正在连接…</h1><p class=\"sub\">设备正在连接你填写的 Wi-Fi，页面会自动刷新。"
            "手机可能会短暂断开本热点，断开后请重新连接 RecBean-Setup 查看结果。</p>"));
    }
}

static esp_err_t get_handler(httpd_req_t *req)
{
    const char *uri = req->uri;
    if (strcmp(uri, "/") == 0 || strncmp(uri, "/?", 2) == 0) return send_setup_page(req);
    if (strcmp(uri, "/status") == 0) return send_status_page(req);
    if (strcmp(uri, "/rescan") == 0) {
        taskENTER_CRITICAL(&s_mux);
        s_rescan = true;
        taskEXIT_CRITICAL(&s_mux);
        if (s_notify) xTaskNotifyGive(s_notify);
        return finish_page(req, send_page(req, "/",
            "<h1>正在扫描…</h1><p class=\"sub\">几秒后自动返回。</p>"));
    }
    // 其他路径（系统联网探测等）一律重定向到配置页，触发“需要登录”弹窗。
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > RB_SETUP_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid");
        return ESP_FAIL;
    }
    char body[RB_SETUP_BODY_MAX + 1];
    int total = 0;
    int retries = 0;
    while (total < (int)req->content_len && retries < 3) {
        int got = httpd_req_recv(req, body + total, req->content_len - (size_t)total);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            retries++;
            continue;
        }
        if (got <= 0) {
            memset(body, 0, sizeof(body));
            return ESP_FAIL;
        }
        total += got;
    }
    if (total != (int)req->content_len) {
        memset(body, 0, sizeof(body));
        httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "timeout");
        return ESP_FAIL;
    }
    body[total] = '\0';
    rb_setup_form_t form;
    bool ok = rb_setup_parse_form(body, (size_t)total, &form);
    memset(body, 0, sizeof(body));   // 正文含明文密码：用完即清
    if (!ok) {
        return finish_page(req, send_page(req, NULL,
            "<h1 class=\"err\">填写有误</h1><p>请选择或输入 Wi-Fi 名称；密码至少 8 位（开放网络留空）；"
            "服务器地址形如 192.168.2.144:8090。</p><p><a href=\"/\">返回</a></p>"));
    }
    taskENTER_CRITICAL(&s_mux);
    s_form = form;
    s_form_ready = true;
    s_state = RB_PORTAL_JOINING;
    taskEXIT_CRITICAL(&s_mux);
    memset(&form, 0, sizeof(form));
    if (s_notify) xTaskNotifyGive(s_notify);
    return send_status_page(req);
}

esp_err_t rb_portal_start(const char *server_prefill, TaskHandle_t notify)
{
    taskENTER_CRITICAL(&s_mux);
    snprintf(s_prefill, sizeof(s_prefill), "%s", server_prefill ? server_prefill : "");
    s_state = RB_PORTAL_WAITING;
    s_form_ready = false;
    s_rescan = false;
    taskEXIT_CRITICAL(&s_mux);
    s_notify = notify;

    if (!s_dns_task) {
        s_dns_stop = false;
        if (xTaskCreate(dns_task, "rb_dns", 3072, NULL, 3, &s_dns_task) != pdPASS) {
            s_dns_task = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_httpd) return ESP_OK;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    // 单用户短时配网：3 个 socket、小 backlog、LRU 清理（参考 phoenixzhc 的实测起点）。
    cfg.max_open_sockets = 3;
    cfg.backlog_conn = 2;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 5;
    cfg.send_wait_timeout = 5;
    cfg.stack_size = 6144;
    cfg.max_uri_handlers = 4;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) {
        s_httpd = NULL;
        rb_portal_stop();
        return err;
    }
    const httpd_uri_t get_uri = {.uri = "/*", .method = HTTP_GET, .handler = get_handler};
    const httpd_uri_t post_uri = {.uri = "/setup", .method = HTTP_POST, .handler = post_handler};
    err = httpd_register_uri_handler(s_httpd, &post_uri);
    if (err == ESP_OK) err = httpd_register_uri_handler(s_httpd, &get_uri);
    if (err != ESP_OK) rb_portal_stop();
    return err;
}

void rb_portal_stop(void)
{
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    if (s_dns_task) {
        s_dns_stop = true;
        for (int i = 0; i < 20 && s_dns_task; i++) vTaskDelay(pdMS_TO_TICKS(100));
    }
    taskENTER_CRITICAL(&s_mux);
    memset(&s_form, 0, sizeof(s_form));
    s_form_ready = false;
    taskEXIT_CRITICAL(&s_mux);
}

bool rb_portal_running(void)
{
    return s_httpd != NULL;
}

static int rssi_desc(const void *a, const void *b)
{
    return ((const rb_scan_ap_t *)b)->rssi - ((const rb_scan_ap_t *)a)->rssi;
}

void rb_portal_set_scan(const rb_scan_ap_t *aps, size_t count)
{
    rb_scan_ap_t merged[RB_PORTAL_SCAN_MAX];
    size_t n = 0;
    for (size_t i = 0; aps && i < count; i++) {
        if (aps[i].ssid[0] == '\0') continue;          // 隐藏网络
        size_t j = 0;
        while (j < n && strcmp(merged[j].ssid, aps[i].ssid) != 0) j++;
        if (j < n) {                                    // 同名多 AP：保留信号最强的
            if (aps[i].rssi > merged[j].rssi) merged[j] = aps[i];
            continue;
        }
        if (n < RB_PORTAL_SCAN_MAX) {
            merged[n++] = aps[i];
            continue;
        }
        size_t weak = 0;
        for (size_t k = 1; k < n; k++) {
            if (merged[k].rssi < merged[weak].rssi) weak = k;
        }
        if (aps[i].rssi > merged[weak].rssi) merged[weak] = aps[i];
    }
    qsort(merged, n, sizeof(merged[0]), rssi_desc);
    taskENTER_CRITICAL(&s_mux);
    memcpy(s_scan, merged, n * sizeof(merged[0]));
    s_scan_count = n;
    taskEXIT_CRITICAL(&s_mux);
}

void rb_portal_set_state(rb_portal_state_t state)
{
    taskENTER_CRITICAL(&s_mux);
    s_state = state;
    taskEXIT_CRITICAL(&s_mux);
}

bool rb_portal_take_form(rb_setup_form_t *out)
{
    bool ready;
    taskENTER_CRITICAL(&s_mux);
    ready = s_form_ready;
    if (ready) {
        *out = s_form;
        memset(&s_form, 0, sizeof(s_form));
        s_form_ready = false;
    }
    taskEXIT_CRITICAL(&s_mux);
    return ready;
}

bool rb_portal_take_rescan(void)
{
    bool v;
    taskENTER_CRITICAL(&s_mux);
    v = s_rescan;
    s_rescan = false;
    taskEXIT_CRITICAL(&s_mux);
    return v;
}
