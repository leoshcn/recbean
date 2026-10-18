// main/rb_session.c —— 录音会话任务实现。行为见 rb_session.h。
#include "rb_session.h"

#include "rb_adpcm.h"
#include "rb_audio.h"
#include "rb_bus.h"
#include "rb_net.h"
#include "rb_proto.h"
#include "rb_setup.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "rb_session";

#define RB_FW_VERSION "0.1.0"

// ---- 会话任务通知位（RB_AUDIO_NOTIFY_BIT = bit0 由 rb_audio 使用）----
// 命令位只用于唤醒；命令本身看 s_req / s_stop_gen / s_cancel_gen 中的会话代号。
#define BIT_CMD_START     (1u << 1)
#define BIT_CMD_STOP      (1u << 2)
#define BIT_CMD_CANCEL    (1u << 3)
#define BIT_WS_CONNECTED  (1u << 4)
#define BIT_WS_DOWN       (1u << 5)   // 断开或被关闭
#define BIT_WS_STARTED    (1u << 6)
#define BIT_WS_SAVED      (1u << 7)
#define BIT_WS_ERROR      (1u << 8)   // 非 asr_error 的 error 消息
#define BIT_WS_HS_FAIL    (1u << 9)   // 握手被 HTTP 拒绝

#define CONNECT_TIMEOUT_MS  6000      // 连接 + 握手
#define STARTED_TIMEOUT_MS  5000      // 发 start 后等 started
#define RESUME_TIMEOUT_MS   5000      // 发 resume 后等 started(resumed)
#define FLUSH_TIMEOUT_MS    20000     // 用户结束后，等链路恢复把缓存发完的上限
#define SAVED_TIMEOUT_MS    25000     // 发 stop 后等 saved（须大于服务端 ASR_FINISH_TIMEOUT_S=15 s）
#define SEND_TIMEOUT_MS     2000      // 单帧发送超时
#define HEAP_LOG_MS         10000     // 录音中内存日志周期

#define PTT_GATE_MARGIN_MS  150       // 笔记：按下满 RB_PTT_MIN_MS + 此余量才发 start（见 run_session 1b）

static TaskHandle_t s_task;

// ---- 命令（app 任务写，会话任务读；都是单个 32 位字，单写者）----
// 每次 start 生成新的会话代号，stop/cancel 记下它们针对的代号。会话任务只认自己代号的
// 命令：上一次会话残留的 cancel 不会误伤新会话；上一次会话收尾期间到达的新 start 也不会
// 随通知位一起被清掉（通知位只用来唤醒）。笔记“快速点一下又马上按住”依赖这一点。
static volatile uint32_t s_req;          // (代号 << 1) | (1 = 笔记)
static volatile uint32_t s_stop_gen;     // 最近一次 stop 针对的代号
static volatile uint32_t s_cancel_gen;   // 最近一次 cancel 针对的代号

// ---- websocket 事件任务写、会话任务读（s_mux 保护）----
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_rx_rec_id[RB_PROTO_ID_MAX + 1];  // 最近一次 started/saved 中的 recording_id
static bool s_rx_has_next;
static uint32_t s_rx_next_seq;
static char s_rx_err_code[RB_PROTO_CODE_MAX + 1];
static rb_err_code_t s_rx_err;
static int64_t s_rx_duration;
static int32_t s_rx_sentences;
static int s_hs_status;
static volatile bool s_session_live;           // 会话进行中：文本事件才转发给 app

// 文本帧重组缓冲：websocket 事件任务独占。服务端单条消息远小于 2 KB。
static char s_rx[2048];
static size_t s_rx_len;
static bool s_rx_overflow;

// 发送帧暂存（会话任务独占，静态分配避免占栈）。
static uint8_t s_tx_frame[RB_ADPCM_FRAME_BYTES];
static char s_headers[RB_PROTO_TOKEN_MAX + 40];
static char s_url[RB_SERVER_CAP + 24];

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void notify(uint32_t bits)
{
    if (s_task) xTaskNotify(s_task, bits, eSetBits);
}

// 解析并分发一条完整的文本消息（运行在 websocket 事件任务中）。
static void handle_text(const char *json, size_t len)
{
    rb_msg_t m;
    if (!rb_proto_parse(json, len, &m)) {
        ESP_LOGW(TAG, "无法解析服务端消息");
        return;
    }
    switch (m.type) {
    case RB_MSG_STARTED:
        taskENTER_CRITICAL(&s_mux);
        memcpy(s_rx_rec_id, m.recording_id, sizeof(s_rx_rec_id));
        s_rx_has_next = m.has_next_seq;
        s_rx_next_seq = (uint32_t)m.next_seq;
        taskEXIT_CRITICAL(&s_mux);
        notify(BIT_WS_STARTED);
        break;
    case RB_MSG_PARTIAL:
        if (s_session_live) rb_bus_text(false, 0, 0, 0, m.text);
        break;
    case RB_MSG_FINAL:
        if (s_session_live) {
            rb_bus_text(true, m.idx, m.speaker, m.start_ms > 0 ? (uint32_t)m.start_ms : 0, m.text);
        }
        break;
    case RB_MSG_ERROR:
        ESP_LOGW(TAG, "服务端错误 %s: %s", m.err_code, m.text);
        if (m.err == RB_ERR_ASR) {
            // 识别失败不影响录音落盘：只提示，继续上传。
            if (s_session_live) rb_bus_event(RB_EV_SESS_ASR_ERROR, 0, 0, 0, 100);
            break;
        }
        taskENTER_CRITICAL(&s_mux);
        memcpy(s_rx_err_code, m.err_code, sizeof(s_rx_err_code));
        s_rx_err = m.err;
        taskEXIT_CRITICAL(&s_mux);
        notify(BIT_WS_ERROR);
        break;
    case RB_MSG_SAVED:
        taskENTER_CRITICAL(&s_mux);
        s_rx_duration = m.duration_ms;
        s_rx_sentences = m.sentences;
        taskEXIT_CRITICAL(&s_mux);
        notify(BIT_WS_SAVED);
        break;
    default:
        break;   // pong 等扩展消息忽略
    }
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    const esp_websocket_event_data_t *ev = (const esp_websocket_event_data_t *)data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        notify(BIT_WS_CONNECTED);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        notify(BIT_WS_DOWN);
        break;
    case WEBSOCKET_EVENT_ERROR:
        if (ev && ev->error_handle.error_type == WEBSOCKET_ERROR_TYPE_HANDSHAKE) {
            s_hs_status = ev->error_handle.esp_ws_handshake_status_code;
            notify(BIT_WS_HS_FAIL);
        }
        break;
    case WEBSOCKET_EVENT_DATA:
        if (!ev || (ev->op_code != 0x1 && ev->op_code != 0x0)) break;   // 只处理文本及其续帧
        if (ev->payload_offset == 0) {
            s_rx_len = 0;
            s_rx_overflow = false;
        }
        if (ev->data_len > 0) {
            if (s_rx_len + (size_t)ev->data_len < sizeof(s_rx)) {
                memcpy(s_rx + s_rx_len, ev->data_ptr, (size_t)ev->data_len);
                s_rx_len += (size_t)ev->data_len;
            } else {
                s_rx_overflow = true;   // 超长消息整条丢弃
            }
        }
        if (ev->fin && ev->payload_offset + ev->data_len >= ev->payload_len) {
            if (!s_rx_overflow) handle_text(s_rx, s_rx_len);
            else ESP_LOGW(TAG, "丢弃超长消息 %d B", ev->payload_len);
            s_rx_len = 0;
        }
        break;
    default:
        break;
    }
}

static uint32_t wait_bits(uint32_t timeout_ms)
{
    uint32_t bits = 0;
    xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(timeout_ms));
    return bits;
}

static bool send_text(esp_websocket_client_handle_t c, const char *s)
{
    int n = (int)strlen(s);
    return esp_websocket_client_send_text(c, s, n, pdMS_TO_TICKS(SEND_TIMEOUT_MS)) == n;
}

static rb_fail_t fail_from_error(void)
{
    rb_err_code_t err;
    taskENTER_CRITICAL(&s_mux);
    err = s_rx_err;
    taskEXIT_CRITICAL(&s_mux);
    if (err == RB_ERR_AUTH) return RB_FAIL_AUTH;
    if (err == RB_ERR_BUSY) return RB_FAIL_BUSY;
    return RB_FAIL_SERVER;
}

// 本会话已被取消，或 app 已经开始了更新的会话（代号变了）：旧会话应尽快收尾，且不再
// 向 app 上报会话事件——会话事件不带代号，迟到的 FAILED/ENDED/SAVED 会被新会话误收。
static bool cancelled(uint32_t gen)
{
    return s_cancel_gen == gen || (s_req >> 1) != gen;
}

// 录音开始前失败：上报 FAILED；令牌被拒时请网络任务清令牌并回到配对。
static void report_failed(uint32_t gen, rb_fail_t reason)
{
    ESP_LOGW(TAG, "会话未能开始，原因 %d", (int)reason);
    if (reason == RB_FAIL_AUTH) rb_net_request_repair();
    if (!cancelled(gen)) rb_bus_event(RB_EV_SESS_FAILED, reason, 0, 0, 500);
}

static void report_ended(uint32_t gen, rb_fail_t reason)
{
    ESP_LOGW(TAG, "录音异常结束，原因 %d", (int)reason);
    if (reason == RB_FAIL_AUTH) rb_net_request_repair();
    if (!cancelled(gen)) rb_bus_event(RB_EV_SESS_ENDED, reason, 0, 0, 500);
}

static bool stop_requested(uint32_t gen)
{
    return s_stop_gen == gen;
}

// 把未发送的帧尽量发出去；遇到发送失败就停（链路断了，等重连）。
static void drain_frames(esp_websocket_client_handle_t c, uint32_t *pending, uint32_t gen)
{
    uint32_t seq;
    while (rb_audio_peek(s_tx_frame, &seq)) {
        int n = esp_websocket_client_send_bin(c, (const char *)s_tx_frame, RB_ADPCM_FRAME_BYTES,
                                              pdMS_TO_TICKS(SEND_TIMEOUT_MS));
        if (n != RB_ADPCM_FRAME_BYTES) break;
        rb_audio_mark_sent(seq);
        // 发送期间到达的事件不能丢：收集起来交给主循环（命令看代号，不在位里）。
        uint32_t more = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &more, 0) == pdTRUE) *pending |= more;
        if (cancelled(gen) || (*pending & BIT_WS_DOWN)) break;
    }
}

// 一次完整的录音会话。返回时 websocket 客户端已销毁、采音已停止。
// 笔记模式（design.md §3.4）：先开始采音再连服务器，帧从 seq 0 进环形缓冲；
// started 之前收到的 stop 只停采音并记住，started 后把缓冲发完再发 stop、等 saved。
static void run_session(uint32_t gen, rb_mode_t mode)
{
    const bool note = mode == RB_MODE_NOTE;
    const int64_t t_begin = now_ms();
    esp_websocket_client_handle_t c = NULL;
    bool audio_on = false;        // 本会话打开过采音（cleanup 负责停止）
    bool net_active = false;
    char msg[160];
    // 清掉上一次会话残留的事件位。命令（start/stop/cancel）按代号判断，不受影响。
    uint32_t pending = 0;
    (void)wait_bits(0);

    if (note) {
        // 按下即采音：codec 唤醒 + 打开通常 < 0.2 s，此后的语音全部进缓冲，开头不丢字。
        if (rb_audio_start() != ESP_OK) {
            report_failed(gen, RB_FAIL_AUDIO);
            return;
        }
        audio_on = true;
        ESP_LOGI(TAG, "笔记：已开始采音，后台连接服务器");
    }

    char server[RB_SERVER_CAP];
    char token[RB_PROTO_TOKEN_MAX + 1];
    if (!rb_net_get_session_config(server, sizeof(server), token, sizeof(token))) {
        report_failed(gen, RB_FAIL_AUTH);   // 没有令牌：需要配对
        goto cleanup;
    }
    rb_net_set_session_active(true);
    net_active = true;
    snprintf(s_headers, sizeof(s_headers), "Authorization: Bearer %s\r\n", token);
    memset(token, 0, sizeof(token));
    (void)rb_url_ws(server, s_url, sizeof(s_url));
    taskENTER_CRITICAL(&s_mux);
    s_rx_rec_id[0] = '\0';
    s_rx_has_next = false;
    s_rx_err = RB_ERR_OTHER;
    s_rx_err_code[0] = '\0';
    taskEXIT_CRITICAL(&s_mux);
    s_hs_status = 0;

    const esp_websocket_client_config_t cfg = {
        .uri = s_url,
        .headers = s_headers,
        .buffer_size = 2048,              // 一帧 1608 B 不分片；单条 JSON 也放得下
        .task_stack = 5120,               // 事件回调里 rb_proto_parse 约 2 KB 栈
        .task_prio = 5,
        .reconnect_timeout_ms = 1000,     // 断线 1 s 后自动重连
        .network_timeout_ms = 5000,
        .ping_interval_sec = 5,           // 协议层 ping，及时发现半开连接
        .pingpong_timeout_sec = 15,
        .enable_close_reconnect = true,   // 服务端正常关闭（如重启）后也自动重连
    };
    c = esp_websocket_client_init(&cfg);
    if (!c) {
        report_failed(gen, RB_FAIL_UNREACHABLE);
        goto cleanup;
    }
    esp_websocket_register_events(c, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    // 初始化期间用户已取消（UI 已回待机）：不再连接。
    if (cancelled(gen)) goto cleanup;

    if (esp_websocket_client_start(c) != ESP_OK) {
        report_failed(gen, RB_FAIL_UNREACHABLE);
        goto cleanup;
    }

    // 笔记：started 之前用户松开 → 立刻停采音（计时已在 UI 冻结），稍后补发缓冲。
    bool audio_released = false;
#define NOTE_PRESTART_CHECKS()                                                        \
    do {                                                                              \
        if (note && !audio_released && stop_requested(gen)) {                         \
            rb_audio_stop();                                                          \
            audio_released = true;                                                    \
            ESP_LOGI(TAG, "笔记：连上服务器前已松开，缓冲 %lu 帧",                      \
                     (unsigned long)rb_audio_backlog());                              \
        }                                                                             \
        if (note && rb_audio_overflowed()) {                                          \
            /* 缓冲装满（约 8 s）仍没连上：放弃本段，服务端还没有这条录音。 */        \
            report_failed(gen, RB_FAIL_UNREACHABLE);                                       \
            goto cleanup;                                                             \
        }                                                                             \
    } while (0)

    // ---- 1. 连接 ----
    int64_t deadline = now_ms() + CONNECT_TIMEOUT_MS;
    for (;;) {
        pending |= wait_bits(100);
        if (cancelled(gen)) goto cleanup;
        if (pending & BIT_WS_CONNECTED) break;
        if (pending & BIT_WS_HS_FAIL) {
            int st = s_hs_status;
            if (st == 401 || st == 403) {
                report_failed(gen, RB_FAIL_AUTH);
                goto cleanup;
            }
            pending &= ~BIT_WS_HS_FAIL;   // 其他握手失败：继续重试到超时
        }
        NOTE_PRESTART_CHECKS();
        if (now_ms() >= deadline) {
            report_failed(gen, RB_FAIL_UNREACHABLE);
            goto cleanup;
        }
    }
    pending &= ~BIT_WS_CONNECTED;
    rb_net_log_heap("ws connected");

    // ---- 1b. 笔记：按住满 RB_PTT_MIN_MS 才发 start ----
    // 误触（短按）会被 app 取消；在那之前不发 start，服务端就不会建这条录音（PRD R1.3）。
    // 音频一直在缓冲里，推迟发 start 不丢字。松开（stop）也立即放行。
    if (note) {
        const int64_t gate = t_begin + RB_PTT_MIN_MS + PTT_GATE_MARGIN_MS;
        while (now_ms() < gate && !stop_requested(gen)) {
            pending |= wait_bits(50);
            if (cancelled(gen)) goto cleanup;
        }
        NOTE_PRESTART_CHECKS();
    }

    // ---- 2. start → started ----
    if (rb_proto_build_start(msg, sizeof(msg), RB_FW_VERSION,
                             note ? RB_PROTO_MODE_NOTE : RB_PROTO_MODE_MEETING) < 0 ||
        !send_text(c, msg)) {
        report_failed(gen, RB_FAIL_UNREACHABLE);
        goto cleanup;
    }
    deadline = now_ms() + STARTED_TIMEOUT_MS;
    for (;;) {
        pending |= wait_bits(100);
        if (cancelled(gen)) goto cleanup;
        if (pending & BIT_WS_STARTED) break;
        if (pending & BIT_WS_ERROR) {
            report_failed(gen, fail_from_error());
            goto cleanup;
        }
        NOTE_PRESTART_CHECKS();
        if (now_ms() >= deadline) {
            report_failed(gen, RB_FAIL_UNREACHABLE);
            goto cleanup;
        }
    }
#undef NOTE_PRESTART_CHECKS
    pending &= ~(BIT_WS_STARTED | BIT_WS_DOWN);
    char rec_id[RB_PROTO_ID_MAX + 1];
    taskENTER_CRITICAL(&s_mux);
    memcpy(rec_id, s_rx_rec_id, sizeof(rec_id));
    taskEXIT_CRITICAL(&s_mux);
    if (rec_id[0] == '\0') {
        report_failed(gen, RB_FAIL_SERVER);
        goto cleanup;
    }

    // ---- 3. 开始采音（会议）/ 已在采音（笔记）----
    if (note) {
        if (rb_audio_overflowed()) {
            // 极少见：恰在 started 到达前溢出。让服务端收尾（空录音），按连不上处理。
            if (rb_proto_build_stop(msg, sizeof(msg)) > 0) (void)send_text(c, msg);
            report_failed(gen, RB_FAIL_UNREACHABLE);
            goto cleanup;
        }
    } else {
        if (rb_audio_start() != ESP_OK) {
            // 服务端已建了录音：发 stop 让它收尾（空录音），再报麦克风失败。
            if (rb_proto_build_stop(msg, sizeof(msg)) > 0) (void)send_text(c, msg);
            report_failed(gen, RB_FAIL_AUDIO);
            goto cleanup;
        }
        audio_on = true;
    }
    if (cancelled(gen)) goto cleanup;
    s_session_live = true;
    ESP_LOGI(TAG, "%s开始 %s，已缓冲 %lu 帧", note ? "笔记" : "录音", rec_id,
             (unsigned long)rb_audio_backlog());
    rb_bus_event(RB_EV_SESS_STARTED, 0, 0, 0, 500);
    rb_net_log_heap("recording start");

    // ---- 4. 上传循环 ----
    bool link_ok = true;
    bool resume_pending = false;
    int64_t resume_deadline = 0;
    bool stopping = false;
    bool stop_sent = false;
    int64_t flush_deadline = 0;
    int64_t saved_deadline = 0;
    int64_t next_heap_log = now_ms() + HEAP_LOG_MS;
    for (;;) {
        pending |= wait_bits(50);
        int64_t now = now_ms();
        if (cancelled(gen)) {
            ESP_LOGW(TAG, "会话被取消");
            break;
        }
        if (stop_requested(gen) && !stopping) {
            rb_audio_stop();   // 先停采音，最后一帧进入缓冲（笔记松开早于 started 时已停，幂等）
            stopping = true;
            flush_deadline = now + FLUSH_TIMEOUT_MS;
            ESP_LOGI(TAG, "用户结束录音，待上传 %lu 帧", (unsigned long)rb_audio_backlog());
        }
        if (pending & BIT_WS_DOWN) {
            if (link_ok) {
                ESP_LOGW(TAG, "链路中断，音频进入缓存");
                link_ok = false;
                rb_bus_event(RB_EV_SESS_LINK, 0, 0, 0, 100);
            }
            resume_pending = false;
        }
        if ((pending & BIT_WS_CONNECTED) && !link_ok) {
            // 重连成功：请求续传。next_seq 填我们还保留的最旧帧，服务端回复它真正需要的序号。
            if (rb_proto_build_resume(msg, sizeof(msg), rec_id, rb_audio_tail()) > 0 && send_text(c, msg)) {
                resume_pending = true;
                resume_deadline = now + RESUME_TIMEOUT_MS;
            }
        }
        if ((pending & BIT_WS_STARTED) && resume_pending) {
            bool has;
            uint32_t next;
            taskENTER_CRITICAL(&s_mux);
            has = s_rx_has_next;
            next = s_rx_next_seq;
            taskEXIT_CRITICAL(&s_mux);
            uint32_t from = rb_audio_rewind(has ? next : rb_audio_tail());
            ESP_LOGI(TAG, "续传成功，从第 %lu 帧重发（服务端期望 %s%lu）", (unsigned long)from,
                     has ? "" : "未给出，按最旧帧 ", (unsigned long)(has ? next : from));
            resume_pending = false;
            link_ok = true;
            rb_bus_event(RB_EV_SESS_LINK, 1, 0, 0, 100);
        }
        if (pending & BIT_WS_SAVED) {
            // 正常结束的回复，或续传时服务端已把录音收尾（resume 超时）。
            int64_t dur;
            int32_t n;
            taskENTER_CRITICAL(&s_mux);
            dur = s_rx_duration;
            n = s_rx_sentences;
            taskEXIT_CRITICAL(&s_mux);
            ESP_LOGI(TAG, "已保存：%lld ms，%ld 句", (long long)dur, (long)n);
            rb_audio_stop();
            if (!cancelled(gen)) rb_bus_event(RB_EV_SESS_SAVED, 0, dur, n, 500);
            break;
        }
        if (pending & BIT_WS_ERROR) {
            report_ended(gen, fail_from_error());   // resume_failed / not_recording / auth 等
            break;
        }
        pending = 0;

        if (!stopping && rb_audio_overflowed()) {
            // 断网超过缓存时长：本次录音结束，服务端把已收部分存为 interrupted。
            rb_audio_stop();
            report_ended(gen, RB_FAIL_OVERFLOW);
            break;
        }
        if (resume_pending && now >= resume_deadline) {
            report_ended(gen, RB_FAIL_SERVER);
            break;
        }
        if (link_ok) drain_frames(c, &pending, gen);
        if (stopping && link_ok && !stop_sent && rb_audio_backlog() == 0) {
            if (rb_proto_build_stop(msg, sizeof(msg)) > 0 && send_text(c, msg)) {
                stop_sent = true;
                saved_deadline = now_ms() + SAVED_TIMEOUT_MS;
            }
        }
        if (stop_sent && now >= saved_deadline) {
            report_ended(gen, RB_FAIL_SAVE_TIMEOUT);
            break;
        }
        if (stopping && !stop_sent && now >= flush_deadline) {
            report_ended(gen, RB_FAIL_OVERFLOW);   // 链路一直没恢复：已上传部分由服务端存为 interrupted
            break;
        }
        if (now >= next_heap_log) {
            next_heap_log = now + HEAP_LOG_MS;
            rb_net_log_heap("recording");
            ESP_LOGI(TAG, "链路 %s，待上传 %lu 帧", link_ok ? "正常" : "中断",
                     (unsigned long)rb_audio_backlog());
        }
    }

cleanup:
    s_session_live = false;
    if (audio_on) rb_audio_stop();
    // enable_close_reconnect 下 close() 不阻塞且会自动重连，因此直接 destroy（内部先 stop）。
    if (c) esp_websocket_client_destroy(c);
    if (net_active) rb_net_set_session_active(false);
    (void)wait_bits(0);   // 只清事件位；收尾期间到达的新 start 记在 s_req 里，不会丢
    rb_net_log_heap("session end");
}

static void session_task(void *arg)
{
    (void)arg;
    uint32_t last_gen = 0;
    for (;;) {
        uint32_t req = s_req;
        uint32_t gen = req >> 1;
        if (gen != last_gen) {
            last_gen = gen;
            // start 之后、任务醒来之前用户已取消：不再开始会话。
            if (!cancelled(gen)) run_session(gen, (req & 1u) ? RB_MODE_NOTE : RB_MODE_MEETING);
            continue;   // 会话期间可能又来了新的 start：回到循环顶部再检查
        }
        (void)xTaskNotifyWait(0, UINT32_MAX, NULL, portMAX_DELAY);
    }
}

esp_err_t rb_session_init(void)
{
    if (s_task) return ESP_OK;
    // 4 KB 栈：消息构造与帧复制用静态缓冲；websocket 收发在客户端自己的任务里。
    if (xTaskCreate(session_task, "rb_sess", 4096, NULL, 5, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return rb_audio_init(s_task);
}

// 下面三个接口只在 app 任务中调用（单写者）：先写代号/模式，再发通知位唤醒会话任务。
void rb_session_start(rb_mode_t mode)
{
    uint32_t gen = ((s_req >> 1) + 1u) & 0x7FFFFFFFu;
    if (gen == 0) gen = 1;   // 0 留给“从未 stop/cancel”
    s_req = (gen << 1) | (mode == RB_MODE_NOTE ? 1u : 0u);
    notify(BIT_CMD_START);
}

void rb_session_stop(void)
{
    s_stop_gen = s_req >> 1;
    notify(BIT_CMD_STOP);
}

void rb_session_cancel(void)
{
    s_cancel_gen = s_req >> 1;
    notify(BIT_CMD_CANCEL);
}

uint32_t rb_session_backlog_ms(void)
{
    return rb_audio_backlog() * RB_ADPCM_FRAME_MS;
}
