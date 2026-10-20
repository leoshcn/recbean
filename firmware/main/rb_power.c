// main/rb_power.c —— 应用层电源编排实现。流程见 rb_power.h。
//
// 外设关闭顺序遵循 docs/reference/shinku-chen/deep-sleep-peripheral-power-off.zh_CN.md；
// 按键脚唤醒陷阱（ADC 占脚时数字电平恒为 0）见
// docs/reference/shinku-chen/landscape-rotation-and-deep-sleep-key-wake.zh_CN.md。
#include "rb_power.h"

#include "rb_bus.h"
#include "rb_net.h"

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "sdkconfig.h"

static const char *TAG = "rb_power";

#define LOW_BATT_SHOW_MS   2000         // 低电深睡前“电量过低”停留
#define DEEP_KEY_RETRY_MS  1000         // 终端阶段按键仍按住时的重试间隔
#define DEEP_KEY_RETRIES   30           // 约 30 s 仍按住（卡键）：放弃深睡改为重启
#define LVGL_LOCK_MS       1000
#define RTC_MAGIC          0x52425057u  // "RBPW"

// 深睡期间保留；冷启动/软件复位时由启动代码重新初始化为 0。
RTC_DATA_ATTR static uint32_t s_rtc_magic;
RTC_DATA_ATTR static uint32_t s_rtc_reason;
RTC_DATA_ATTR static uint32_t s_rtc_count;   // 连续深睡-唤醒次数（诊断用）

static bool s_light;                // 浅睡步骤已执行
static bool s_lvgl_stopped;
static TaskHandle_t s_lvgl_task;    // 浅睡中被挂起的 LVGL 任务（NULL = 未挂起）
static bool s_display_sleeping;
static bool s_buttons_suspended;
static volatile bool s_wake_isr;    // 暂停期间 GPIO 唤醒中断已触发（有键按下过）

static const char *reason_name(rb_sleep_reason_t r)
{
    switch (r) {
    case RB_SLEEP_IDLE: return "熄屏无操作";
    case RB_SLEEP_PROV: return "配网页无操作";
    case RB_SLEEP_LOW_BATT: return "电量过低";
    case RB_SLEEP_NONE:
    default: return "无";
    }
}

// GPIO 中断上下文：只置标志并入队。
static void on_wake_isr(void *user)
{
    (void)user;
    s_wake_isr = true;
    (void)rb_bus_event_from_isr(RB_EV_WAKE_KEY);
}

// 活动态不降频（背光 LEDC、LCD SPI 时序与改动前一致）；只在浅睡中允许降频与自动 light sleep。
static void pm_set_light_sleep(bool enable)
{
#if CONFIG_PM_ENABLE
    esp_pm_config_t cfg = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = enable ? CONFIG_XTAL_FREQ : CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
#ifdef RB_POWER_DEBUG_NO_SLEEP
        .light_sleep_enable = false,
#else
        .light_sleep_enable = enable,
#endif
    };
    esp_err_t e = esp_pm_configure(&cfg);
    if (e != ESP_OK) ESP_LOGW(TAG, "esp_pm_configure 失败: %s", esp_err_to_name(e));
#else
    (void)enable;
#endif
}

// 暂停 LVGL（持 bsp_lvgl_lock() 调用）。lvgl_port_stop() 只停 tick 定时器并禁用 lv_timer，
// 而 LVGL 9 的 lv_timer_handler() 在禁用时立即返回 1：taskLVGL 会每 1~2 ms 醒一次，
// CPU 根本进不了自动 light sleep。所以还要挂起该任务；持锁时它不在 LVGL 调用里，挂起是安全的。
static void lvgl_pause(void)
{
    if (!s_lvgl_stopped) s_lvgl_stopped = lvgl_port_stop() == ESP_OK;
    if (!s_lvgl_task) {
        s_lvgl_task = xTaskGetHandle("taskLVGL");   // esp_lvgl_port 2.x 的任务名
        if (s_lvgl_task) vTaskSuspend(s_lvgl_task);
        else ESP_LOGW(TAG, "找不到 LVGL 任务，浅睡中无法挂起");
    }
}

// 唤醒屏幕（持 bsp_lvgl_lock() 调用）：Sleep Out → 恢复 LVGL 任务与定时器 → 强制全屏重绘。
static void screen_wake(void)
{
    if (s_display_sleeping && bsp_display_sleep(false) == ESP_OK) s_display_sleeping = false;
    if (s_lvgl_task) {
        vTaskResume(s_lvgl_task);
        s_lvgl_task = NULL;
    }
    if (s_lvgl_stopped && lvgl_port_resume() == ESP_OK) s_lvgl_stopped = false;
    lv_obj_invalidate(lv_screen_active());
}

static void try_suspend_buttons(void)
{
    if (s_buttons_suspended) return;
    s_wake_isr = false;
    esp_err_t e = bsp_button_suspend(on_wake_isr, NULL);
    if (e == ESP_OK) {
        s_buttons_suspended = true;
    } else if (e == ESP_ERR_INVALID_STATE) {
        ESP_LOGD(TAG, "按键按住或未就绪，下个节拍再暂停按键");
    } else {
        ESP_LOGW(TAG, "按键暂停失败: %s", esp_err_to_name(e));
    }
}

static void try_resume_buttons(void)
{
    if (!s_buttons_suspended) return;
    esp_err_t e = bsp_button_resume();
    if (e == ESP_OK) s_buttons_suspended = false;
    else ESP_LOGE(TAG, "按键恢复失败，下个节拍重试: %s", esp_err_to_name(e));
}

rb_sleep_reason_t rb_power_boot(void)
{
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    rb_sleep_reason_t reason = s_rtc_magic == RTC_MAGIC ? (rb_sleep_reason_t)s_rtc_reason : RB_SLEEP_NONE;
    s_rtc_magic = 0;
    s_rtc_reason = RB_SLEEP_NONE;
    if (cause == ESP_SLEEP_WAKEUP_GPIO) {
        ESP_LOGI(TAG, "深睡被按键唤醒（GPIO%d），入睡原因：%s，连续第 %u 次", BSP_BTN_GPIO,
                 reason_name(reason), (unsigned)s_rtc_count);
        return reason;
    }
    ESP_LOGI(TAG, "启动：唤醒原因 %d，复位原因 %d", (int)cause, (int)esp_reset_reason());
    s_rtc_count = 0;
    return RB_SLEEP_NONE;
}

bool rb_power_light_active(void)
{
    return s_light;
}

void rb_power_light_enter(void)
{
    if (s_light) return;
    s_light = true;
    ESP_LOGI(TAG, "进入浅睡（熄屏）");
    if (bsp_lvgl_lock(LVGL_LOCK_MS)) {
        // 持锁发 SLPIN 时 panel IO 会先等排队的刷屏传输完成。
        lvgl_pause();
        if (!s_display_sleeping) s_display_sleeping = bsp_display_sleep(true) == ESP_OK;
        bsp_lvgl_unlock();
    } else {
        ESP_LOGW(TAG, "拿不到 LVGL 锁，本次浅睡不暂停刷屏");
    }
    try_suspend_buttons();
    rb_net_set_low_power(true);
    pm_set_light_sleep(true);
}

void rb_power_light_exit(void)
{
    if (!s_light) return;
    s_light = false;
    pm_set_light_sleep(false);
    try_resume_buttons();
    rb_net_set_low_power(false);
    if (bsp_lvgl_lock(LVGL_LOCK_MS)) {
        // 先 Sleep Out（含 120 ms 稳定时间），再恢复 LVGL 并标记全屏重绘；调用方等 LVGL 任务刷完一帧后再开背光。
        screen_wake();
        bsp_lvgl_unlock();
    } else {
        ESP_LOGW(TAG, "拿不到 LVGL 锁，屏幕稍后恢复");
    }
    ESP_LOGI(TAG, "退出浅睡");
}

void rb_power_poll(void)
{
    if (s_light) {
        try_suspend_buttons();
        return;
    }
    try_resume_buttons();
    if ((s_display_sleeping || s_lvgl_stopped || s_lvgl_task) && bsp_lvgl_lock(0)) {
        screen_wake();
        bsp_lvgl_unlock();
    }
}

// 入睡前（尚未关闭任何外设）检查是否有键按着。
static bool key_pressed_now(void)
{
    if (s_buttons_suspended) return s_wake_isr;   // 暂停中：唤醒中断已触发 = 有键按下过
    static const uint16_t win[BSP_BTN_COUNT][2] = BSP_BTN_MV_TABLE;
    int mv = bsp_button_read_mv();
    return mv >= 0 && mv < win[BSP_BTN_COUNT - 1][1];
}

bool rb_power_enter_deep(rb_sleep_reason_t reason)
{
    if (key_pressed_now()) {
        ESP_LOGW(TAG, "有按键按着，推迟深睡（%s）", reason_name(reason));
        return false;
    }
    ESP_LOGW(TAG, "进入深睡：%s", reason_name(reason));
    if (reason == RB_SLEEP_LOW_BATT) vTaskDelay(pdMS_TO_TICKS(LOW_BATT_SHOW_MS));
    pm_set_light_sleep(false);   // 关闭过程不被自动 light sleep 打断
    s_rtc_magic = RTC_MAGIC;
    s_rtc_reason = reason;
    s_rtc_count++;

    // 1. 无线：本产品让 Wi-Fi 长期存活，必须在 LCD 步骤之前停掉。
    rb_net_stop();

    // 2. 阻止 LVGL 刷屏：持锁直到入睡，不再释放。
    if (!bsp_lvgl_lock(LVGL_LOCK_MS)) ESP_LOGW(TAG, "拿不到 LVGL 锁，仍继续关闭外设");

    // 3. 板载外设，顺序固定；单项失败只记日志，不阻止后续步骤。
    if (bsp_battery_sleep() != ESP_OK) ESP_LOGW(TAG, "CW2017 休眠失败");
    if (bsp_audio_sleep() != ESP_OK) ESP_LOGW(TAG, "ES8311 休眠失败");
    if (bsp_audio_prepare_deep_sleep() != ESP_OK) ESP_LOGW(TAG, "I2S 引脚释放失败");
    if (bsp_i2c_prepare_deep_sleep() != ESP_OK) ESP_LOGW(TAG, "I2C 引脚释放失败");
    if (bsp_display_prepare_deep_sleep() != ESP_OK) ESP_LOGW(TAG, "LCD 深睡准备失败");

    // 4. 按键脚：释放 ADC、数字输入上拉、武装低电平唤醒；按住则等松开（否则睡下即醒）。
    int level = -1;
    for (int attempt = 0;; attempt++) {
        esp_err_t e = bsp_button_prepare_deep_sleep(&level);
        if (e == ESP_OK) break;
        if (attempt >= DEEP_KEY_RETRIES) {
            ESP_LOGE(TAG, "按键脚持续为低（电平 %d）或配置失败，放弃深睡并重启", level);
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(DEEP_KEY_RETRY_MS));
    }

    // 5. 入睡。I2S/I2C 已脱离，被拒绝（唤醒源已触发）时不尝试恢复，直接重启。
    ESP_LOGI(TAG, "深睡中，按任意键唤醒（GPIO%d 电平 %d）", BSP_BTN_GPIO, level);
    esp_err_t e = esp_deep_sleep_try_to_start();
    ESP_LOGW(TAG, "深睡被拒绝（%s），重启", esp_err_to_name(e));
    esp_restart();
    return true;
}
