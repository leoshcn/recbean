// main/main.c —— 录音豆（RecBean）固件入口与 app 任务。
//
// 任务划分（design.md §7）：
//   app 任务  rb_app      优先级 5  事件队列消费者：状态机、转写缓冲、全部 LVGL 更新（持锁）
//   网络任务  rb_net      优先级 4  Wi-Fi / 配网 / 配对 / 服务器探测
//   会话任务  rb_sess     优先级 5  WebSocket 会话与音频上传
//   采集任务  rb_audio    优先级 6  ES8311 读取 + ADPCM 编码
//   以及 LVGL 渲染任务（esp_lvgl_port）、websocket 客户端任务、按键 esp_timer 回调。
// 按键回调只把事件放进队列（不阻塞、不碰 LVGL），所有慢操作在对应任务中完成。
#include "rb_app.h"
#include "rb_bus.h"
#include "rb_fonts.h"
#include "rb_net.h"
#include "rb_session.h"
#include "rb_store.h"
#include "rb_transcript.h"
#include "rb_ui.h"

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "recbean";

#define RB_FW_VERSION      "0.1.0"
#define QUEUE_DEPTH        10       // 每项约 300 B → 约 3 KB
#define TICK_MS            250      // 状态机 TICK 周期（计时、超时、背光）
#define HEAP_LOG_MS        10000    // 周期堆日志间隔
#define BATTERY_POLL_MS    10000    // 电量计读取周期
#define LVGL_LOCK_MS       200      // app 任务等 LVGL 锁的上限；拿不到就下一轮再画

static QueueHandle_t s_queue;       // rb_post_t：所有任务 → app 任务
static volatile bool s_ready;       // UI 建好之前丢弃按键
static bool s_battery_ok;           // 电量计初始化成功

// app 任务独占的状态（静态分配；转写缓冲约 8 KB）。
static rb_app_t s_app;
static rb_transcript_t s_ts;
static rb_ui_info_t s_info;
static rb_post_t s_rx;              // 出队暂存，避免占用 app 任务栈

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// ---------------------------------------------------------------------------
// rb_bus 实现（可在任意任务调用；按键回调传 timeout 0）
// ---------------------------------------------------------------------------

bool rb_bus_event(rb_ev_type_t type, int32_t a, int64_t b, int32_t c, uint32_t timeout_ms)
{
    if (!s_queue) return false;
    rb_post_t item;
    item.kind = RB_POST_EVENT;
    item.ev.type = type;
    item.ev.a = a;
    item.ev.b = b;
    item.ev.c = c;
    item.text[0] = '\0';
    return xQueueSend(s_queue, &item, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

bool rb_bus_text(bool final, int32_t idx, int32_t speaker, uint32_t start_ms, const char *text)
{
    if (!s_queue || !text) return false;
    rb_post_t item;
    item.kind = final ? RB_POST_FINAL : RB_POST_PARTIAL;
    item.idx = idx;
    item.speaker = speaker;
    item.start_ms = start_ms;
    size_t n = strnlen(text, RB_TS_TEXT_MAX);
    memcpy(item.text, text, n);
    item.text[n] = '\0';
    // 定稿句不能丢：多等一会儿；进行中句子丢了无所谓（下一条会覆盖）。
    return xQueueSend(s_queue, &item, pdMS_TO_TICKS(final ? 500 : 20)) == pdTRUE;
}

// 按键回调：运行在 button 组件的 esp_timer 任务中，只入队、立即返回。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (!s_ready) return;
    rb_ev_type_t type;
    if (ev == BSP_BTN_CLICK) {
        if (btn == BSP_BTN_UP) type = RB_EV_KEY_UP;
        else if (btn == BSP_BTN_DOWN) type = RB_EV_KEY_DOWN;
        else type = RB_EV_KEY_OK;
    } else if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        type = RB_EV_KEY_OK_LONG;
    } else {
        return;   // 按下/双击/上下长按不使用
    }
    (void)rb_bus_event(type, 0, 0, 0, 0);
}

// ---------------------------------------------------------------------------
// app 任务
// ---------------------------------------------------------------------------

static void refresh_info(uint32_t now, bool force_battery)
{
    static uint32_t next_battery;
    rb_net_get_status(&s_info.net);
    if (s_battery_ok && (force_battery || (int32_t)(now - next_battery) >= 0)) {
        next_battery = now + BATTERY_POLL_MS;
        s_info.battery_soc = bsp_battery_soc();   // I2C 读取约 1 ms；读失败返回 -1
        s_info.battery_mv = bsp_battery_mv();
    }
    s_info.backlog_ms = rb_session_backlog_ms();
    s_info.free_heap = (uint32_t)esp_get_free_heap_size();
    s_info.largest_block = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
}

static void run_actions(uint32_t act)
{
    if (act & RB_ACT_CLEAR_TRANSCRIPT) rb_transcript_reset(&s_ts);
    if (act & RB_ACT_SESSION_START) rb_session_start();
    if (act & RB_ACT_SESSION_STOP) rb_session_stop();
    if (act & RB_ACT_SESSION_CANCEL) rb_session_cancel();
    if (act & RB_ACT_REPROVISION) rb_net_request_reprovision();
    if (act & RB_ACT_BACKLIGHT) bsp_display_backlight(s_app.backlight);
}

// 处理一条队列项，返回动作位。
static uint32_t dispatch(const rb_post_t *p, uint32_t now)
{
    if (p->kind == RB_POST_EVENT) return rb_app_handle(&s_app, &p->ev, now);
    // 转写文本只在录音会话中接受（会话结束后的迟到消息丢弃）。
    if (!rb_app_in_session(&s_app)) return 0;
    if (p->kind == RB_POST_PARTIAL) {
        rb_transcript_set_partial(&s_ts, p->text, strlen(p->text));
        return RB_ACT_RENDER;
    }
    if (!rb_transcript_add_final(&s_ts, p->idx, p->speaker, p->start_ms, p->text, strlen(p->text))) {
        return 0;
    }
    rb_event_t ev = {.type = RB_EV_TRANSCRIPT, .c = (int32_t)rb_transcript_count(&s_ts)};
    return rb_app_handle(&s_app, &ev, now) | RB_ACT_RENDER;
}

static void log_heap(const char *where);

static void app_task(void *arg)
{
    (void)arg;
    uint32_t next_tick = now_ms();
    uint32_t next_heap_log = next_tick + HEAP_LOG_MS;
    bool dirty = true;
    for (;;) {
        uint32_t now = now_ms();
        if ((int32_t)(now - next_heap_log) >= 0) {
            // LVGL 与 Wi-Fi 共用系统堆，周期记录便于真机判断余量与泄漏。
            log_heap("periodic");
            next_heap_log = now + HEAP_LOG_MS;
        }
        int32_t wait = (int32_t)(next_tick - now);
        if (wait < 0) wait = 0;
        uint32_t act = 0;
        if (xQueueReceive(s_queue, &s_rx, pdMS_TO_TICKS((uint32_t)wait)) == pdTRUE) {
            act |= dispatch(&s_rx, now_ms());
            // 一次把队列里积压的都处理掉，再统一重绘一次。
            while (xQueueReceive(s_queue, &s_rx, 0) == pdTRUE) act |= dispatch(&s_rx, now_ms());
        }
        now = now_ms();
        if ((int32_t)(now - next_tick) >= 0) {
            rb_event_t tick = {.type = RB_EV_TICK};
            act |= rb_app_handle(&s_app, &tick, now);
            next_tick = now + TICK_MS;
        }
        run_actions(act);
        if (act & RB_ACT_RENDER) dirty = true;
        if (!dirty) continue;
        refresh_info(now, false);
        if (bsp_lvgl_lock(LVGL_LOCK_MS)) {
            rb_ui_render(&s_app, &s_ts, &s_info, now);
            bsp_lvgl_unlock();
            dirty = false;
        }
    }
}

// ---------------------------------------------------------------------------
// 启动
// ---------------------------------------------------------------------------

static void log_heap(const char *where)
{
    ESP_LOGI("rb_mem", "[%s] free=%u largest=%u min_free=%u", where,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)esp_get_minimum_free_heap_size());
}

void app_main(void)
{
    ESP_LOGI(TAG, "录音豆 RecBean 固件 %s 启动", RB_FW_VERSION);
    log_heap("boot");

    (void)bsp_i2c_init();
    // 屏幕是唯一的交互载体：失败时打印接线信息后停止（没有串口菜单降级）。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败（MOSI=%d SCLK=%d CS=%d DC=%d BL=%d）",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(RB_BACKLIGHT_FULL);

    s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(rb_post_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "事件队列创建失败");
        return;
    }
    s_battery_ok = bsp_battery_init() == ESP_OK;
    if (!s_battery_ok) ESP_LOGW(TAG, "电量计不可用，顶栏不显示电量");
    s_info.battery_soc = -1;
    s_info.battery_mv = -1;
    s_info.fw_version = RB_FW_VERSION;

    // 存储 → 网络（确定初始阶段）→ 音频 → 会话。音频失败不阻止配网，开始录音时会提示。
    esp_err_t store_err = rb_store_init();
    if (store_err != ESP_OK) ESP_LOGE(TAG, "NVS 不可用，配置无法保存: %s", esp_err_to_name(store_err));
    if (bsp_audio_init() != ESP_OK) ESP_LOGE(TAG, "音频 codec 初始化失败");
    if (rb_net_start() != ESP_OK) ESP_LOGE(TAG, "网络任务启动失败");
    if (rb_session_init() != ESP_OK) ESP_LOGE(TAG, "会话/采集任务启动失败");

    rb_transcript_reset(&s_ts);
    rb_app_init(&s_app, rb_net_initial_phase(), now_ms());

    if (bsp_lvgl_lock(1000)) {
        rb_fonts_init();
        (void)rb_fonts_selfcheck();
        rb_ui_init();
        refresh_info(now_ms(), true);
        rb_ui_render(&s_app, &s_ts, &s_info, now_ms());
        bsp_lvgl_unlock();
    }
    log_heap("ui ready");

    // 4.5 KB 栈：渲染时 snprintf 设备信息（~0.5 KB 局部缓冲）+ LVGL 布局调用链。
    if (xTaskCreate(app_task, "rb_app", 4608, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "app 任务创建失败");
        return;
    }
    if (bsp_button_init(on_key, NULL) != ESP_OK) ESP_LOGE(TAG, "按键初始化失败");
    s_ready = true;
}
