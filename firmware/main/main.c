// main/main.c —— 录音豆（RecBean）固件入口与 app 任务。
//
// 任务划分（design.md §7）：
//   app 任务  rb_app      优先级 5  事件队列消费者：状态机、转写缓冲、全部 LVGL 更新（持锁）
//   网络任务  rb_net      优先级 4  Wi-Fi / 配网 / 配对 / 服务器探测
//   会话任务  rb_sess     优先级 5  WebSocket 会话与音频上传
//   采集任务  rb_audio    优先级 6  ES8311 读取 + ADPCM 编码
//   以及 LVGL 渲染任务（esp_lvgl_port）、websocket 客户端任务、按键 esp_timer 回调。
// 按键回调只把事件放进队列（不阻塞、不碰 LVGL），所有慢操作在对应任务中完成。
// 两级省电（rb_power.h）：浅睡时 app 任务节拍放慢到 1 s 且不重绘；深睡由 app 任务执行。
#include "rb_app.h"
#include "rb_bus.h"
#include "rb_fonts.h"
#include "rb_net.h"
#include "rb_power.h"
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
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <string.h>

static const char *TAG = "recbean";

#define RB_FW_VERSION      "0.1.0"
#define QUEUE_DEPTH        10       // 每项约 300 B → 约 3 KB
#define TICK_MS            250      // 状态机 TICK 周期（计时、超时、背光）
#define TICK_LIGHT_MS      1000     // 浅睡中的 TICK 周期（减少唤醒；深睡计时精度足够）
#define HEAP_LOG_MS        10000    // 周期堆日志间隔（浅睡中不打）
#define BATTERY_POLL_MS    60000    // 电量计读取周期（与是否重绘无关，浅睡中也读，供低电保护）
#define BATTERY_RETRY_MS   5000     // 读数无效（电量计刚重启、SOC 尚未算出）时亮屏下的重读间隔
#define BATTERY_BOOT_GAP_MS 200     // 低电唤醒后两次确认读数的间隔
#define LVGL_LOCK_MS       200
#define WAKE_FRAME_MS      300      // 浅睡唤醒：等 LVGL 刷完第一帧再开背光的上限      // app 任务等 LVGL 锁的上限；拿不到就下一轮再画

static QueueHandle_t s_queue;       // rb_post_t：所有任务 → app 任务
static SemaphoreHandle_t s_refr_done;   // LVGL 任务刷完一帧（LV_EVENT_REFR_READY）
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

bool rb_bus_event_from_isr(rb_ev_type_t type)
{
    static rb_post_t s_isr_item;    // 约 300 B，放静态区而不是中断栈
    if (!s_queue) return false;
    s_isr_item.kind = RB_POST_EVENT;
    s_isr_item.ev.type = type;
    s_isr_item.ev.a = 0;
    s_isr_item.ev.b = 0;
    s_isr_item.ev.c = 0;
    s_isr_item.text[0] = '\0';
    BaseType_t woken = pdFALSE;
    bool ok = xQueueSendFromISR(s_queue, &s_isr_item, &woken) == pdTRUE;
    if (woken == pdTRUE) portYIELD_FROM_ISR();
    return ok;
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
    if (btn == BSP_BTN_UP && ev == BSP_BTN_PRESS) {
        type = RB_EV_PTT_DOWN;     // 按住说话（录音笔记）
    } else if (btn == BSP_BTN_UP && ev == BSP_BTN_RELEASE) {
        type = RB_EV_PTT_UP;
    } else if (ev == BSP_BTN_CLICK) {
        if (btn == BSP_BTN_UP) type = RB_EV_KEY_UP;
        else if (btn == BSP_BTN_DOWN) type = RB_EV_KEY_DOWN;
        else type = RB_EV_KEY_OK;
    } else if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        type = RB_EV_KEY_OK_LONG;
    } else {
        return;   // 下/确定的按下与松开、双击、上下长按不使用
    }
    (void)rb_bus_event(type, 0, 0, 0, 0);
}

// ---------------------------------------------------------------------------
// app 任务
// ---------------------------------------------------------------------------

static void refresh_info(void)
{
    rb_net_get_status(&s_info.net);
    // 只在会话中有意义；低电唤醒再睡的路径上会话/采集任务尚未初始化，不能碰它的锁。
    s_info.backlog_ms = rb_app_in_session(&s_app) ? rb_session_backlog_ms() : 0;
    s_info.free_heap = (uint32_t)esp_get_free_heap_size();
    s_info.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    s_info.largest_block = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
}

// 读一次电量计（I2C 约 1 ms；读失败返回 -1），过滤后送入状态机做低电判定。
// 电压正常而 SOC=0 的读数无效：顶栏保留上一次的有效值（没有就不显示），不显示 0%。
// 返回状态机动作，显示值变化时附带 RB_ACT_RENDER。
static uint32_t read_battery(uint32_t now)
{
    if (!s_battery_ok) return 0;
    int mv = bsp_battery_mv();
    int raw = bsp_battery_soc();
    int soc = rb_app_filter_soc(raw, mv);
    if (soc < 0 && raw == 0) ESP_LOGW(TAG, "电量计读数 0%%（%d mV）不可信，忽略", mv);
    rb_event_t ev = {.type = RB_EV_BATTERY, .a = soc, .c = mv};
    uint32_t act = rb_app_handle(&s_app, &ev, now);
    // 电压每次都有毫伏级抖动：只在电量数字变化时重绘；设备信息页的电压随其他重绘更新。
    int shown = soc >= 0 ? soc : s_info.battery_soc;
    if (shown != s_info.battery_soc) act |= RB_ACT_RENDER;
    s_info.battery_soc = shown;
    if (mv > 0) s_info.battery_mv = mv;
    return act;
}

// 定时读电量：与是否重绘解耦，待机不动也每 BATTERY_POLL_MS 刷新一次。
static uint32_t poll_battery(uint32_t now)
{
    static uint32_t next_battery;
    static bool started;
    if (started && (int32_t)(now - next_battery) < 0) return 0;
    started = true;
    uint32_t act = read_battery(now);
    // 顶栏还没有有效电量（开机时电量计未就绪）：亮屏期间尽快重读，不等满 60 s。
    bool retry = s_battery_ok && s_info.battery_soc < 0 && !rb_power_light_active();
    next_battery = now + (retry ? BATTERY_RETRY_MS : BATTERY_POLL_MS);
    return act;
}

static void render_wake_frame(void);

// 一批事件里可能先进后出（或反过来），浅睡的执行以状态机的最终 power 为准，动作位只作触发。
static void run_actions(uint32_t act)
{
    // 退出浅睡最先做：屏幕 Sleep Out、LVGL 恢复之后再开背光（避免残影）。
    // 深睡（power=DEEP）时只有低电提示需要亮屏（带 LIGHT_EXIT），其余保持浅睡现状直接入睡。
    if (rb_power_light_active() &&
        ((act & RB_ACT_POWER_LIGHT_EXIT) || s_app.power == RB_POWER_ACTIVE)) {
        rb_power_light_exit();
        render_wake_frame();
    }
    if (act & RB_ACT_CLEAR_TRANSCRIPT) rb_transcript_reset(&s_ts);
    if (act & RB_ACT_SESSION_START) rb_session_start(s_app.mode);
    if (act & RB_ACT_SESSION_STOP) rb_session_stop();
    if (act & RB_ACT_SESSION_CANCEL) rb_session_cancel();
    if (act & RB_ACT_REPROVISION) rb_net_request_reprovision();
    if (act & RB_ACT_BACKLIGHT) bsp_display_backlight(s_app.backlight);
    // 进入浅睡最后做：背光已灭。
    if (s_app.power == RB_POWER_LIGHT && !rb_power_light_active()) rb_power_light_enter();
}

static bool render_now(uint32_t now)
{
    refresh_info();
    if (!bsp_lvgl_lock(LVGL_LOCK_MS)) return false;
    rb_ui_render(&s_app, &s_ts, &s_info, now);
    bsp_lvgl_unlock();
    return true;
}

// 运行在 LVGL 任务中：一帧渲染完成。
static void on_refr_ready(lv_event_t *e)
{
    (void)e;
    xSemaphoreGive(s_refr_done);
}

// 退出浅睡后、开背光前：更新界面并等 LVGL 任务刷完一帧，否则背光一亮会先闪出
// 面板 GRAM 里入睡前的旧帧。渲染必须留在 LVGL 任务（7 KB 栈）里做：在 app 任务（4.5 KB 栈）
// 里调 lv_refr_now() 整屏绘制会栈溢出，真机上表现为每次唤醒都 panic 重启。
static void render_wake_frame(void)
{
    refresh_info();
    if (!s_refr_done || !bsp_lvgl_lock(LVGL_LOCK_MS)) return;
    rb_ui_render(&s_app, &s_ts, &s_info, now_ms());
    (void)xSemaphoreTake(s_refr_done, 0);   // 丢掉入睡前残留的信号
    bsp_lvgl_unlock();
    (void)xSemaphoreTake(s_refr_done, pdMS_TO_TICKS(WAKE_FRAME_MS));
}

// 执行深睡（RB_ACT_POWER_DEEP）。正常不返回；按键按着时放弃，交回状态机继续运行。
static uint32_t run_deep_sleep(void)
{
    if (s_app.deep_reason == RB_SLEEP_LOW_BATT) {
        // “电量过低”提示必须真的画到屏上：等 LVGL 刷完这一帧（LVGL 任务约 20 ms 一次）。
        (void)render_now(now_ms());
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (rb_power_enter_deep(s_app.deep_reason)) return 0;
    rb_event_t ab = {.type = RB_EV_DEEP_ABORT};
    return rb_app_handle(&s_app, &ab, now_ms()) | RB_ACT_RENDER;
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

// 笔记录音中 PTT_UP 丢失的兜底（例如转写消息把队列塞满时按键回调入队失败）：
// 每个 TICK 读一次按键电平，连续 PTT_RELEASE_TICKS 次不在“上”键窗口内就补发 PTT_UP。
// 正常的松开事件经按键组件去抖后几十毫秒内就到，总是先于这里生效。
#define PTT_RELEASE_TICKS 2
static uint32_t check_ptt_release(uint32_t now)
{
    static const uint16_t win[BSP_BTN_COUNT][2] = BSP_BTN_MV_TABLE;
    static uint8_t misses;
    if (!rb_app_ptt_waiting_release(&s_app)) {
        misses = 0;
        return 0;
    }
    int mv = bsp_button_read_mv();
    // 读失败时按“仍按着”处理，交给会话/状态机自身的超时。
    bool held = mv < 0 || (mv >= win[BSP_BTN_UP][0] && mv < win[BSP_BTN_UP][1]);
    if (held) {
        misses = 0;
        return 0;
    }
    if (++misses < PTT_RELEASE_TICKS) return 0;
    misses = 0;
    ESP_LOGW(TAG, "上键松开事件丢失（%d mV），按电平补发", mv);
    rb_event_t up = {.type = RB_EV_PTT_UP};
    return rb_app_handle(&s_app, &up, now);
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
            // LVGL 与 Wi-Fi 共用系统堆，周期记录便于真机判断余量与泄漏（浅睡中省掉）。
            if (!rb_power_light_active()) log_heap("periodic");
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
            act |= check_ptt_release(now);
            rb_event_t tick = {.type = RB_EV_TICK};
            act |= rb_app_handle(&s_app, &tick, now);
            act |= poll_battery(now);
            rb_power_poll();
            next_tick = now + (s_app.power == RB_POWER_LIGHT ? TICK_LIGHT_MS : TICK_MS);
        }
        run_actions(act);
        // 刚从浅睡回来：立即恢复 250 ms 节拍（计时、超时依赖它）。
        if ((act & RB_ACT_POWER_LIGHT_EXIT) && s_app.power == RB_POWER_ACTIVE) next_tick = now_ms();
        if (act & RB_ACT_RENDER) dirty = true;
        if (act & RB_ACT_POWER_DEEP) {
            uint32_t back = run_deep_sleep();   // 只有放弃深睡时才返回
            run_actions(back);
            dirty = true;
            continue;
        }
        // 浅睡中屏幕已 Sleep In、LVGL 暂停：不重绘，醒来后统一画一次。
        if (!dirty || rb_power_light_active()) continue;
        if (render_now(now)) dirty = false;
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
    rb_sleep_reason_t woke_from = rb_power_boot();   // 打印唤醒原因；深睡唤醒时返回入睡原因

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
    s_info.reset_reason = (int)esp_reset_reason();

    // 低电保护深睡后被唤醒（R9）：电量仍低就只显示提示并再次深睡，不启动 Wi-Fi 与音频
    // （ES8311 在上次深睡前已 suspend，3.3 V 常通电源轨下保持该状态）。
    if (woke_from == RB_SLEEP_LOW_BATT && s_battery_ok) {
        rb_app_init(&s_app, RB_NET_READY, now_ms());
        (void)read_battery(now_ms());
        vTaskDelay(pdMS_TO_TICKS(BATTERY_BOOT_GAP_MS));
        (void)read_battery(now_ms());
        if (s_app.power == RB_POWER_DEEP) {
            ESP_LOGW(TAG, "电量仍过低（%d%%，%d mV），提示后再次深睡", s_info.battery_soc, s_info.battery_mv);
            rb_transcript_reset(&s_ts);
            if (bsp_lvgl_lock(1000)) {
                rb_fonts_init();
                rb_ui_init();
                bsp_lvgl_unlock();
            }
            bsp_display_backlight(s_app.backlight);
            (void)run_deep_sleep();   // 正常不返回
        }
        ESP_LOGI(TAG, "电量已恢复（%d%%，%d mV），正常启动", s_info.battery_soc, s_info.battery_mv);
    }

    // 存储 → 网络（确定初始阶段）→ 音频 → 会话。音频失败不阻止配网，开始录音时会提示。
    esp_err_t store_err = rb_store_init();
    if (store_err != ESP_OK) ESP_LOGE(TAG, "NVS 不可用，配置无法保存: %s", esp_err_to_name(store_err));
    if (bsp_audio_init() != ESP_OK) ESP_LOGE(TAG, "音频 codec 初始化失败");
    if (rb_net_start() != ESP_OK) ESP_LOGE(TAG, "网络任务启动失败");
    if (rb_session_init() != ESP_OK) ESP_LOGE(TAG, "会话/采集任务启动失败");

    rb_transcript_reset(&s_ts);
    rb_app_init(&s_app, rb_net_initial_phase(), now_ms());
    (void)poll_battery(now_ms());   // 首次读数（并开始 60 s 周期）

    if (bsp_lvgl_lock(1000)) {
        rb_fonts_init();
        (void)rb_fonts_selfcheck();
        rb_ui_init();
        s_refr_done = xSemaphoreCreateBinary();
        if (s_refr_done) {
            lv_display_add_event_cb(lv_display_get_default(), on_refr_ready, LV_EVENT_REFR_READY, NULL);
        }
        refresh_info();
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
