// main/rb_net.c —— 网络任务实现。流程见 rb_net.h。
//
// Wi-Fi/SoftAP 初始化顺序、APSTA 配网、加入失败判定参考 ai-passport
// feature/xhs-dashboard 分支 main/xhs_net.c；配对协议改为 RecBean 服务端的
// POST /device/pair + GET /device/pair/{id}（server/README.md）。
#include "rb_net.h"

#include "rb_bus.h"
#include "rb_store.h"

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "rb_net";
static const char *AP_SSID = "RecBean-Setup";

#define JOIN_TIMEOUT_MS     20000   // 配网提交后等待拿到 IP 的上限
#define AP_HOLD_MS          10000   // 连上后保留热点，让手机看到“已连上”页面
#define PAIR_POLL_MS        3000    // 配对轮询间隔
#define PAIR_RETRY_MS       5000    // 服务器不可达时的重试间隔
#define PROBE_INTERVAL_MS   15000   // 待机时服务器探测间隔
#define PROBE_TIMEOUT_MS    2500    // 单次 TCP 探测超时
#define HTTP_TIMEOUT_MS     5000
#define SCAN_FETCH_MAX      24
#define LOOP_MS             200     // 网络任务轮询间隔
#define LOOP_LOW_POWER_MS   1000    // 浅睡中的轮询间隔
// STA 的 listen_interval（单位：信标间隔，约 102 ms）：只在 WIFI_PS_MAX_MODEM（浅睡）下生效；
// 活动态的 MIN_MODEM 按 DTIM 醒来，不受影响。真机上取 10（约 1 s）时浅睡中会掉线，收敛到 3。
#define STA_LISTEN_INTERVAL 3

// ---- 由 s_mux 保护的共享状态（网络任务写；app/会话任务读）----
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static rb_config_t s_cfg;          // 当前配置（含明文密码与令牌，只在本模块内保存）
static rb_net_status_t s_st;       // 对外状态快照

// ---- 只由网络任务使用 ----
static TaskHandle_t s_task;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static bool s_wifi_inited;
static bool s_wifi_started;
static rb_setup_form_t s_attempt;  // 正在尝试的配网表单
static int64_t s_join_deadline;
static int64_t s_ap_hold_until;    // > 0 表示热点在“已连上”展示期
static int64_t s_next_pair;
static int64_t s_next_probe;
static int64_t s_next_reconnect;   // > 0 表示计划重连
static int s_backoff_ms;

// ---- 事件回调写、网络任务读的标志 ----
static volatile bool s_ev_got_ip;
static volatile bool s_ev_disconnected;
static volatile uint8_t s_last_reason;
static volatile bool s_got_ip;
static volatile bool s_req_reprov;
static volatile bool s_req_repair;
static volatile bool s_session_active;
static volatile bool s_low_power;  // 浅睡：最大省电、暂停探测
static volatile bool s_stopped;    // 深睡前已停止 Wi-Fi
static bool s_ps_max;              // 当前已下发的是 WIFI_PS_MAX_MODEM（只由网络任务写）
static rb_net_phase_t s_initial_phase;

// 临界区内只用这个有界拷贝（不调用 snprintf 等可能取锁的库函数）。
static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = strnlen(src, cap - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

void rb_net_log_heap(const char *where)
{
    ESP_LOGI("rb_mem", "[%s] free=%u largest=%u min_free=%u", where,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)esp_get_minimum_free_heap_size());
}

static void set_phase(rb_net_phase_t phase)
{
    taskENTER_CRITICAL(&s_mux);
    bool changed = s_st.phase != phase;
    s_st.phase = phase;
    taskEXIT_CRITICAL(&s_mux);
    if (changed) {
        ESP_LOGI(TAG, "阶段 → %d", (int)phase);
        rb_bus_event(RB_EV_NET_PHASE, phase, 0, 0, 200);
    }
}

static rb_net_phase_t phase(void)
{
    taskENTER_CRITICAL(&s_mux);
    rb_net_phase_t p = s_st.phase;
    taskEXIT_CRITICAL(&s_mux);
    return p;
}

// 更新服务器可达状态；变化时（或 force）通知 app 重绘。
static void set_server_ok(int8_t ok, bool force)
{
    taskENTER_CRITICAL(&s_mux);
    bool changed = s_st.server_ok != ok;
    s_st.server_ok = ok;
    taskEXIT_CRITICAL(&s_mux);
    if (changed || force) rb_bus_event(RB_EV_SERVER, ok > 0 ? 1 : 0, 0, 0, 100);
}

static void set_wifi_up(bool up, const char *ip)
{
    taskENTER_CRITICAL(&s_mux);
    bool changed = s_st.wifi_up != up;
    s_st.wifi_up = up;
    copy_str(s_st.ip, sizeof(s_st.ip), up && ip ? ip : "");
    if (!up) s_st.server_ok = -1;
    taskEXIT_CRITICAL(&s_mux);
    if (changed) rb_bus_event(RB_EV_WIFI, up ? 1 : 0, 0, 0, 200);
}

// ---------------------------------------------------------------------------
// Wi-Fi 事件（运行在默认事件循环任务中：只置标志并唤醒网络任务）
// ---------------------------------------------------------------------------

static char s_ip_text[16];

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        s_last_reason = data ? ((const wifi_event_sta_disconnected_t *)data)->reason : 0;
        s_got_ip = false;
        s_ev_disconnected = true;
    } else if (id == WIFI_EVENT_AP_STACONNECTED) {
        taskENTER_CRITICAL(&s_mux);
        s_st.phone_joined = true;
        taskEXIT_CRITICAL(&s_mux);
        rb_bus_event(RB_EV_SERVER, 0, 0, 0, 0);   // 仅触发配网页重绘（配网阶段服务器状态无意义）
    }
    if (s_task) xTaskNotifyGive(s_task);
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == IP_EVENT_STA_GOT_IP && data) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
        snprintf(s_ip_text, sizeof(s_ip_text), IPSTR, IP2STR(&ev->ip_info.ip));
        s_got_ip = true;
        s_ev_got_ip = true;
    }
    if (s_task) xTaskNotifyGive(s_task);
}

static esp_err_t ensure_wifi(void)
{
    if (s_wifi_inited) return ESP_OK;
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_sta_netif) return ESP_ERR_NO_MEM;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) return err;
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL);
    if (err == ESP_OK) err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL);
    // 凭据由我们自己的 NVS 命名空间管理，不让驱动再存一份。
    if (err == ESP_OK) err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) return err;
    s_wifi_inited = true;
    rb_net_log_heap("wifi init");
    return ESP_OK;
}

static esp_err_t apply_sta_config(const char *ssid, const char *password)
{
    wifi_config_t sta;
    memset(&sta, 0, sizeof(sta));
    size_t sn = strnlen(ssid, sizeof(sta.sta.ssid));
    size_t pn = strnlen(password, sizeof(sta.sta.password));
    memcpy(sta.sta.ssid, ssid, sn);
    memcpy(sta.sta.password, password, pn);
    sta.sta.threshold.authmode = pn == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    sta.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    sta.sta.listen_interval = STA_LISTEN_INTERVAL;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta);
    memset(&sta, 0, sizeof(sta));
    return err;
}

static esp_err_t wifi_start_mode(wifi_mode_t mode)
{
    esp_err_t err = esp_wifi_set_mode(mode);
    if (err != ESP_OK) return err;
    if (!s_wifi_started) {
        err = esp_wifi_start();
        if (err != ESP_OK) return err;
        s_wifi_started = true;
    }
    return ESP_OK;
}

static void station_connect(void)
{
    rb_config_t cfg;
    taskENTER_CRITICAL(&s_mux);
    cfg = s_cfg;
    taskEXIT_CRITICAL(&s_mux);
    if (ensure_wifi() != ESP_OK || wifi_start_mode(WIFI_MODE_STA) != ESP_OK ||
        apply_sta_config(cfg.ssid, cfg.password) != ESP_OK) {
        ESP_LOGE(TAG, "STA 启动失败");
    } else {
        (void)esp_wifi_set_ps(WIFI_PS_MIN_MODEM);   // 省电；8 KB/s 上传不需要关闭省电
        s_ps_max = false;
        (void)esp_wifi_connect();
    }
    memset(&cfg, 0, sizeof(cfg));
}

// ---------------------------------------------------------------------------
// 配网
// ---------------------------------------------------------------------------

static void scan_networks(void)
{
    wifi_scan_config_t sc = {.show_hidden = false};
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {   // 阻塞约 2~3 s
        ESP_LOGW(TAG, "扫描失败");
        rb_portal_set_scan(NULL, 0);
        return;
    }
    uint16_t n = SCAN_FETCH_MAX;
    wifi_ap_record_t *recs = calloc(n, sizeof(*recs));
    if (!recs) {
        (void)esp_wifi_clear_ap_list();
        rb_portal_set_scan(NULL, 0);
        return;
    }
    if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) n = 0;
    rb_scan_ap_t *aps = calloc(n ? n : 1, sizeof(*aps));
    if (aps) {
        for (uint16_t i = 0; i < n; i++) {
            memcpy(aps[i].ssid, recs[i].ssid, sizeof(aps[i].ssid) - 1);
            aps[i].rssi = recs[i].rssi;
            aps[i].open = recs[i].authmode == WIFI_AUTH_OPEN;
        }
        rb_portal_set_scan(aps, n);
        free(aps);
    }
    free(recs);
    ESP_LOGI(TAG, "扫描到 %u 个网络", (unsigned)n);
}

static void enter_portal(void)
{
    if (ensure_wifi() != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 初始化失败，无法配网");
        return;
    }
    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();
    wifi_config_t ap;
    memset(&ap, 0, sizeof(ap));
    memcpy(ap.ap.ssid, AP_SSID, strlen(AP_SSID));
    ap.ap.ssid_len = (uint8_t)strlen(AP_SSID);
    ap.ap.channel = 1;
    ap.ap.authmode = WIFI_AUTH_OPEN;   // 短时开放热点：只用于填表，成功后立即关闭
    ap.ap.max_connection = 1;          // 单用户配网，节省内存
    ap.ap.beacon_interval = 100;
    if (wifi_start_mode(WIFI_MODE_APSTA) != ESP_OK || esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK) {
        ESP_LOGE(TAG, "热点启动失败");
        return;
    }
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
    s_ps_max = false;
    scan_networks();
    char server[RB_SERVER_CAP];
    taskENTER_CRITICAL(&s_mux);
    copy_str(server, sizeof(server), s_cfg.server[0] ? s_cfg.server : RB_DEFAULT_SERVER);
    s_st.portal = RB_PORTAL_WAITING;
    s_st.phone_joined = false;
    taskEXIT_CRITICAL(&s_mux);
    if (rb_portal_start(server, s_task) != ESP_OK) ESP_LOGE(TAG, "配网页启动失败");
    s_ap_hold_until = 0;
    rb_net_log_heap("portal start");
}

static void leave_portal(void)
{
    rb_portal_stop();
    if (s_wifi_started) {
        (void)esp_wifi_set_mode(WIFI_MODE_STA);
        (void)esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        s_ps_max = false;
    }
    s_ap_hold_until = 0;
    rb_net_log_heap("portal stop");
}

static void set_portal_state(rb_portal_state_t st)
{
    rb_portal_set_state(st);
    taskENTER_CRITICAL(&s_mux);
    s_st.portal = st;
    taskEXIT_CRITICAL(&s_mux);
    rb_bus_event(RB_EV_SERVER, 0, 0, 0, 100);   // 触发配网页重绘
}

static void begin_join(void)
{
    if (!rb_portal_take_form(&s_attempt)) return;
    ESP_LOGI(TAG, "尝试连接 Wi-Fi（服务器 %s）", s_attempt.server);
    set_phase(RB_NET_PROV_JOINING);
    set_portal_state(RB_PORTAL_JOINING);
    s_ev_disconnected = false;
    s_got_ip = false;
    s_join_deadline = now_ms() + JOIN_TIMEOUT_MS;
    (void)esp_wifi_disconnect();
    if (apply_sta_config(s_attempt.ssid, s_attempt.password) != ESP_OK || esp_wifi_connect() != ESP_OK) {
        s_join_deadline = now_ms();   // 立即判失败
    }
}

static void finish_join(bool ok)
{
    if (!ok) {
        ESP_LOGW(TAG, "配网连接失败 reason=%u", (unsigned)s_last_reason);
        (void)esp_wifi_disconnect();
        memset(&s_attempt, 0, sizeof(s_attempt));
        set_portal_state(RB_PORTAL_FAILED);
        set_phase(RB_NET_PROV_AP);
        return;
    }
    if (rb_store_save_wifi(s_attempt.ssid, s_attempt.password, s_attempt.server) != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi 配置写入 NVS 失败（本次运行仍可使用）");
    }
    taskENTER_CRITICAL(&s_mux);
    memcpy(s_cfg.ssid, s_attempt.ssid, sizeof(s_cfg.ssid));
    memcpy(s_cfg.password, s_attempt.password, sizeof(s_cfg.password));
    memcpy(s_cfg.server, s_attempt.server, sizeof(s_cfg.server));
    s_cfg.token[0] = '\0';
    memcpy(s_st.ssid, s_attempt.ssid, sizeof(s_st.ssid));
    memcpy(s_st.server, s_attempt.server, sizeof(s_st.server));
    s_st.pair_code[0] = '\0';
    taskEXIT_CRITICAL(&s_mux);
    memset(&s_attempt, 0, sizeof(s_attempt));
    set_portal_state(RB_PORTAL_JOINED);
    s_ap_hold_until = now_ms() + AP_HOLD_MS;
    s_next_pair = 0;
    set_phase(RB_NET_PAIRING);
}

// ---------------------------------------------------------------------------
// HTTP（配对）
// ---------------------------------------------------------------------------

// 发一个 JSON 请求。返回 HTTP 状态码，网络失败返回 -1。响应正文写入 out（NUL 结尾，截断时返回 -2）。
static int http_json(esp_http_client_method_t method, const char *url, const char *body,
                     char *out, size_t cap)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 768,
        .buffer_size_tx = 512,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    int status = -1;
    int body_len = body ? (int)strlen(body) : 0;
    if (body) esp_http_client_set_header(c, "Content-Type", "application/json");
    if (esp_http_client_open(c, body_len) != ESP_OK) goto done;
    if (body_len > 0 && esp_http_client_write(c, body, body_len) != body_len) goto close;
    if (esp_http_client_fetch_headers(c) < 0) goto close;
    status = esp_http_client_get_status_code(c);
    size_t used = 0;
    while (used + 1 < cap) {
        int got = esp_http_client_read(c, out + used, (int)(cap - 1 - used));
        if (got < 0) {
            status = -1;
            break;
        }
        if (got == 0) break;
        used += (size_t)got;
    }
    out[used] = '\0';
    if (used + 1 >= cap) {
        char extra;
        if (esp_http_client_read(c, &extra, 1) > 0) status = -2;   // 回复超过上限：拒绝
    }
close:
    esp_http_client_close(c);
done:
    esp_http_client_cleanup(c);
    return status;
}

static void pair_step(void)
{
    char server[RB_SERVER_CAP];
    char id[sizeof(s_st.device_id)];
    char code[RB_PROTO_PAIR_CODE + 1];
    taskENTER_CRITICAL(&s_mux);
    copy_str(server, sizeof(server), s_cfg.server[0] ? s_cfg.server : RB_DEFAULT_SERVER);
    memcpy(id, s_st.device_id, sizeof(id));
    memcpy(code, s_st.pair_code, sizeof(code));
    taskEXIT_CRITICAL(&s_mux);

    char url[160];
    char resp[384];
    rb_pair_reply_t reply;
    if (code[0] == '\0') {
        char body[96];
        if (!rb_url_http(server, "/device/pair", url, sizeof(url)) ||
            rb_proto_build_pair_request(body, sizeof(body), id) < 0) {
            return;
        }
        int st = http_json(HTTP_METHOD_POST, url, body, resp, sizeof(resp));
        if (st != 200 || !rb_proto_parse_pair(resp, strlen(resp), &reply) || !reply.has_code) {
            ESP_LOGW(TAG, "申请配对码失败 status=%d", st);
            set_server_ok(0, false);
            s_next_pair = now_ms() + PAIR_RETRY_MS;
            return;
        }
        taskENTER_CRITICAL(&s_mux);
        memcpy(s_st.pair_code, reply.code, sizeof(s_st.pair_code));
        taskEXIT_CRITICAL(&s_mux);
        ESP_LOGI(TAG, "配对码 %s", reply.code);
        set_server_ok(1, true);
        s_next_pair = now_ms() + PAIR_POLL_MS;
        return;
    }

    char path[64];
    snprintf(path, sizeof(path), "/device/pair/%s", id);
    if (!rb_url_http(server, path, url, sizeof(url))) return;
    int st = http_json(HTTP_METHOD_GET, url, NULL, resp, sizeof(resp));
    if (st == 404) {
        // 配对已过期 / 服务端重启 / 令牌已被取走但没收到：重新申请配对码。
        ESP_LOGI(TAG, "配对记录不存在，重新申请");
        taskENTER_CRITICAL(&s_mux);
        s_st.pair_code[0] = '\0';
        taskEXIT_CRITICAL(&s_mux);
        set_server_ok(1, true);
        s_next_pair = now_ms();
        return;
    }
    if (st != 200 || !rb_proto_parse_pair(resp, strlen(resp), &reply)) {
        memset(resp, 0, sizeof(resp));
        set_server_ok(0, false);
        s_next_pair = now_ms() + PAIR_RETRY_MS;
        return;
    }
    memset(resp, 0, sizeof(resp));
    set_server_ok(1, false);
    s_next_pair = now_ms() + PAIR_POLL_MS;
    if (!reply.has_token) return;   // pending

    // 令牌只下发一次：先写 NVS，失败也继续用内存中的令牌（重启后需重新配对）。
    if (rb_store_save_token(reply.token) != ESP_OK) ESP_LOGW(TAG, "令牌写入 NVS 失败");
    taskENTER_CRITICAL(&s_mux);
    memcpy(s_cfg.token, reply.token, sizeof(s_cfg.token));
    s_st.pair_code[0] = '\0';
    taskEXIT_CRITICAL(&s_mux);
    memset(&reply, 0, sizeof(reply));
    ESP_LOGI(TAG, "配对完成");
    s_next_probe = 0;
    set_phase(RB_NET_READY);
}

// ---------------------------------------------------------------------------
// 服务器探测：只做 TCP 连接（不依赖任何 HTTP 路由，协议外零耦合）。
// ---------------------------------------------------------------------------

static bool probe_server(void)
{
    char server[RB_SERVER_CAP];
    taskENTER_CRITICAL(&s_mux);
    copy_str(server, sizeof(server), s_cfg.server[0] ? s_cfg.server : RB_DEFAULT_SERVER);
    taskEXIT_CRITICAL(&s_mux);
    char *colon = strrchr(server, ':');
    if (!colon) return false;
    *colon = '\0';
    const char *port = colon + 1;

    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res = NULL;
    if (getaddrinfo(server, port, &hints, &res) != 0 || !res) return false;
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    bool ok = false;
    if (s >= 0) {
        int flags = fcntl(s, F_GETFL, 0);
        (void)fcntl(s, F_SETFL, flags | O_NONBLOCK);
        int r = connect(s, res->ai_addr, res->ai_addrlen);
        if (r == 0) {
            ok = true;
        } else if (errno == EINPROGRESS) {
            fd_set wf;
            FD_ZERO(&wf);
            FD_SET(s, &wf);
            struct timeval tv = {.tv_sec = PROBE_TIMEOUT_MS / 1000,
                                 .tv_usec = (PROBE_TIMEOUT_MS % 1000) * 1000};
            if (select(s + 1, NULL, &wf, NULL, &tv) > 0) {
                int soerr = 0;
                socklen_t len = sizeof(soerr);
                ok = getsockopt(s, SOL_SOCKET, SO_ERROR, &soerr, &len) == 0 && soerr == 0;
            }
        }
        close(s);
    }
    freeaddrinfo(res);
    return ok;
}

// ---------------------------------------------------------------------------
// 请求处理与主循环
// ---------------------------------------------------------------------------

static void handle_requests(void)
{
    if (s_req_reprov) {
        s_req_reprov = false;
        ESP_LOGI(TAG, "重新配网：清除 Wi-Fi/服务器/令牌");
        if (rb_store_clear_all() != ESP_OK) ESP_LOGW(TAG, "清除 NVS 失败");
        taskENTER_CRITICAL(&s_mux);
        memset(&s_cfg, 0, sizeof(s_cfg));
        s_st.ssid[0] = '\0';
        copy_str(s_st.server, sizeof(s_st.server), RB_DEFAULT_SERVER);
        s_st.pair_code[0] = '\0';
        taskEXIT_CRITICAL(&s_mux);
        s_next_reconnect = 0;
        if (s_wifi_started) (void)esp_wifi_disconnect();
        set_wifi_up(false, NULL);
        set_phase(RB_NET_PROV_AP);
        enter_portal();
    }
    if (s_req_repair) {
        s_req_repair = false;
        ESP_LOGW(TAG, "服务端拒绝令牌，重新配对");
        if (rb_store_clear_token() != ESP_OK) ESP_LOGW(TAG, "清除令牌失败");
        taskENTER_CRITICAL(&s_mux);
        s_cfg.token[0] = '\0';
        s_st.pair_code[0] = '\0';
        taskEXIT_CRITICAL(&s_mux);
        s_next_pair = 0;
        if (phase() == RB_NET_READY) set_phase(RB_NET_PAIRING);
    }
}

static void handle_wifi_events(void)
{
    rb_net_phase_t p = phase();
    if (s_ev_got_ip) {
        s_ev_got_ip = false;
        ESP_LOGI(TAG, "已连上 Wi-Fi，IP %s", s_ip_text);
        set_wifi_up(true, s_ip_text);
        s_backoff_ms = 0;
        s_next_reconnect = 0;
        s_next_probe = 0;
        rb_net_log_heap("wifi up");
    }
    if (s_ev_disconnected) {
        s_ev_disconnected = false;
        if (p == RB_NET_PROV_JOINING) {
            // 配网尝试中：凭据错误立即判失败，其他原因继续重试直到超时。
            if (rb_wifi_reason_is_final(s_last_reason)) s_join_deadline = now_ms();
            else (void)esp_wifi_connect();
            return;
        }
        if (p == RB_NET_PROV_AP) return;   // 热点阶段 STA 未使用
        ESP_LOGW(TAG, "Wi-Fi 断开 reason=%u", (unsigned)s_last_reason);
        taskENTER_CRITICAL(&s_mux);
        s_st.disc_count++;
        s_st.disc_reason = s_last_reason;
        taskEXIT_CRITICAL(&s_mux);
        set_wifi_up(false, NULL);
        // 指数退避重连：1, 2, 4, 8, 10 s …
        s_backoff_ms = s_backoff_ms == 0 ? 1000 : (s_backoff_ms * 2 > 10000 ? 10000 : s_backoff_ms * 2);
        s_next_reconnect = now_ms() + s_backoff_ms;
    }
    if (s_next_reconnect > 0 && now_ms() >= s_next_reconnect &&
        (p == RB_NET_PAIRING || p == RB_NET_READY)) {
        s_next_reconnect = 0;
        (void)esp_wifi_connect();
    }
}

static void net_task(void *arg)
{
    (void)arg;
    if (phase() == RB_NET_PROV_AP) enter_portal();
    else station_connect();

    for (;;) {
        if (s_stopped) {
            // 深睡前已停 Wi-Fi：不再重连、配对或探测，等待入睡。
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        handle_requests();
        handle_wifi_events();
        int64_t now = now_ms();
        rb_net_phase_t p = phase();

        if (s_ap_hold_until > 0 && now >= s_ap_hold_until) leave_portal();

        // 浅睡省电：只在纯 STA 阶段切换（热点配网与“已连上”展示期间保持原模式）。
        bool want_max = s_low_power;
        if (want_max != s_ps_max && s_wifi_started && s_ap_hold_until == 0 &&
            (p == RB_NET_PAIRING || p == RB_NET_READY)) {
            (void)esp_wifi_set_ps(want_max ? WIFI_PS_MAX_MODEM : WIFI_PS_MIN_MODEM);
            s_ps_max = want_max;
            ESP_LOGI(TAG, "Wi-Fi 省电模式 → %s", want_max ? "MAX_MODEM" : "MIN_MODEM");
            // 亮屏时若正在退避等待重连，立即重连并清零退避，避免用户等上 10 s。
            if (!want_max && s_next_reconnect > 0) {
                s_backoff_ms = 0;
                s_next_reconnect = now;
            }
        }

        switch (p) {
        case RB_NET_PROV_AP:
            if (rb_portal_take_rescan()) scan_networks();
            begin_join();
            break;
        case RB_NET_PROV_JOINING:
            if (s_got_ip) finish_join(true);
            else if (now >= s_join_deadline) finish_join(false);
            break;
        case RB_NET_PAIRING:
            if (s_got_ip && now >= s_next_pair) pair_step();
            break;
        case RB_NET_READY:
            if (s_got_ip && !s_session_active && !s_low_power && now >= s_next_probe) {
                bool ok = probe_server();
                set_server_ok(ok ? 1 : 0, false);
                s_next_probe = now_ms() + PROBE_INTERVAL_MS;
            }
            break;
        }
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(s_low_power ? LOOP_LOW_POWER_MS : LOOP_MS));
    }
}

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------

esp_err_t rb_net_start(void)
{
    if (s_task) return ESP_OK;
    rb_config_t cfg;
    if (rb_store_load(&cfg) != ESP_OK) memset(&cfg, 0, sizeof(cfg));
    uint8_t mac[6] = {0};
    (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);

    taskENTER_CRITICAL(&s_mux);
    s_cfg = cfg;
    memset(&s_st, 0, sizeof(s_st));
    s_st.server_ok = -1;
    rb_device_id_from_mac(mac, s_st.device_id, sizeof(s_st.device_id));
    memcpy(s_st.ssid, cfg.ssid, sizeof(s_st.ssid));
    copy_str(s_st.server, sizeof(s_st.server), cfg.server[0] ? cfg.server : RB_DEFAULT_SERVER);
    if (cfg.ssid[0] == '\0') s_st.phase = RB_NET_PROV_AP;
    else if (cfg.token[0] == '\0') s_st.phase = RB_NET_PAIRING;
    else s_st.phase = RB_NET_READY;
    s_initial_phase = s_st.phase;
    taskEXIT_CRITICAL(&s_mux);
    memset(&cfg, 0, sizeof(cfg));
    ESP_LOGI(TAG, "设备 %s，初始阶段 %d", s_st.device_id, (int)s_initial_phase);

    // 6 KB 栈：esp_http_client、getaddrinfo 与扫描都在本任务中运行（局部缓冲合计 < 1 KB）。
    if (xTaskCreate(net_task, "rb_net", 6144, NULL, 4, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

rb_net_phase_t rb_net_initial_phase(void)
{
    return s_initial_phase;
}

void rb_net_get_status(rb_net_status_t *out)
{
    if (!out) return;
    taskENTER_CRITICAL(&s_mux);
    *out = s_st;
    taskEXIT_CRITICAL(&s_mux);
}

bool rb_net_get_session_config(char *server, size_t server_cap, char *token, size_t token_cap)
{
    bool ok;
    taskENTER_CRITICAL(&s_mux);
    ok = s_st.phase == RB_NET_READY && s_cfg.token[0] != '\0';
    if (ok) {
        copy_str(server, server_cap, s_cfg.server[0] ? s_cfg.server : RB_DEFAULT_SERVER);
        copy_str(token, token_cap, s_cfg.token);
    }
    taskEXIT_CRITICAL(&s_mux);
    return ok;
}

void rb_net_request_reprovision(void)
{
    s_req_reprov = true;
    if (s_task) xTaskNotifyGive(s_task);
}

void rb_net_request_repair(void)
{
    s_req_repair = true;
    if (s_task) xTaskNotifyGive(s_task);
}

void rb_net_set_session_active(bool active)
{
    s_session_active = active;
    if (!active) s_next_probe = 0;
    if (s_task) xTaskNotifyGive(s_task);
}

void rb_net_set_low_power(bool on)
{
    if (s_low_power == on) return;
    s_low_power = on;
    if (!on) s_next_probe = 0;   // 亮屏后立即探测一次，待机页尽快显示真实服务器状态
    if (s_task) xTaskNotifyGive(s_task);
}

void rb_net_stop(void)
{
    s_stopped = true;
    if (s_task) xTaskNotifyGive(s_task);
    if (!s_wifi_started) return;
    esp_err_t e = esp_wifi_disconnect();
    if (e != ESP_OK && e != ESP_ERR_WIFI_NOT_CONNECT) ESP_LOGW(TAG, "断开 Wi-Fi 失败: %s", esp_err_to_name(e));
    e = esp_wifi_stop();
    if (e != ESP_OK) ESP_LOGW(TAG, "停止 Wi-Fi 失败: %s", esp_err_to_name(e));
    s_wifi_started = false;
    ESP_LOGI(TAG, "Wi-Fi 已停止（准备深睡）");
}
