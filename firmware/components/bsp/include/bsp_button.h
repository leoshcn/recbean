// components/bsp/include/bsp_button.h
// 三个按键共用一个 ADC 引脚,靠分压电阻区分。电压窗口见 bsp_pins.h。
#pragma once

#include "esp_err.h"

// 按键索引。数量用 bsp_pins.h 的 BSP_BTN_COUNT(硬件属性,归引脚表管),
// 这里不再定义尾项计数,避免出现 BSP_BTN_COUNT / BSP_BTN_COUNT_ 两个近似名字。
typedef enum {
    BSP_BTN_UP = 0,
    BSP_BTN_DOWN,
    BSP_BTN_OK,
} bsp_btn_t;

typedef enum {
    BSP_BTN_PRESS = 0,   // 按下瞬间(低延迟,适合游戏类即时响应)
    BSP_BTN_CLICK,       // 单击(按下并抬起)
    BSP_BTN_DOUBLE,      // 双击
    BSP_BTN_LONG,        // 长按
    BSP_BTN_RELEASE,     // 松开(任何一次按下之后都会上报,适合按住说话)
} bsp_btn_ev_t;

// 按键事件回调。运行于 button 组件使用的共享 esp_timer 任务,只能入队或执行同等级
// 的有界操作；勿在其中阻塞、访问 LVGL 或做重活。
typedef void (*bsp_btn_cb_t)(bsp_btn_t btn, bsp_btn_ev_t ev, void *user);

// 成功调用可重复，并更新回调与 user；失败会回滚本次已创建的按键和 ADC 资源。
// ADC 校准失败时返回错误而不是把无效电压解码为按键，修正故障后可重试。
esp_err_t bsp_button_init(bsp_btn_cb_t cb, void *user);

// 读当前 ADC 原始电压(mV)。松开时约 3300;按住某键时约为该键的分压值。
// ★ 换了分压/上拉阻值后,用它测出自己的三档电压,再改 bsp_pins.h 的 BSP_BTN_MV_TABLE。
// 读取失败或按键处于 bsp_button_suspend() 暂停期间返回 -1。
int bsp_button_read_mv(void);

// ---------------------------------------------------------------------------
// 睡眠支持。ADC 占用 GPIO0 时该脚的数字电平恒读 0（即使有外部上拉），低电平唤醒会
// 立刻成立，所以两种睡眠都必须先释放 ADC、把引脚交回数字输入（内部上拉 + 外部 10k）。
// 三键分压 0/300/595 mV 都低于数字低电平阈值，任意键都能唤醒。
// 见 docs/reference/shinku-chen/landscape-rotation-and-deep-sleep-key-wake.zh_CN.md。
// 以下接口只能在同一个任务中串行调用（不可在按键回调里调用）。
// ---------------------------------------------------------------------------

// 浅睡唤醒回调：运行在 GPIO 中断上下文，只能做 ISR 安全的有界操作（置标志、
// xQueueSendFromISR 等）。每次 suspend 之后最多调用一次（中断随即关闭）。
typedef void (*bsp_btn_wake_cb_t)(void *user);

// 【可逆】light sleep 前暂停按键：停止 button 组件的轮询定时器，释放 ADC unit 与校准，
// GPIO0 切为数字输入上拉，武装低电平 GPIO 唤醒（esp_sleep_enable_gpio_wakeup）与中断。
// 暂停期间不产生按键事件。回读到按键正按着时恢复原状并返回 ESP_ERR_INVALID_STATE，
// 调用方稍后重试（否则会“睡下即醒”）。未初始化返回 ESP_ERR_INVALID_STATE；已暂停返回 ESP_OK。
esp_err_t bsp_button_suspend(bsp_btn_wake_cb_t cb, void *user);

// 【可逆】light sleep 返回后调用：解除 GPIO 唤醒与中断，重建 ADC unit/校准，恢复按键轮询。
// 未暂停时返回 ESP_OK。重建 ADC 失败时保持暂停并返回错误，可重试。
// 若此时按键仍按着，button 组件恢复后会照常上报 PRESS（例如按住说话）。
esp_err_t bsp_button_resume(void);

// 【终端】deep sleep 前调用：停止按键轮询并释放 ADC（已暂停时只解除浅睡唤醒），GPIO0 切为
// 数字输入上拉，回读电平写入 *level（可为 NULL）；松开（1）时武装
// esp_deep_sleep_enable_gpio_wakeup(GPIO0, 低电平) 并返回 ESP_OK。按住（0）时不武装、返回
// ESP_ERR_INVALID_STATE：调用方应等待松开后重试，不能直接入睡。之后不再上报按键事件，
// 应随即 esp_deep_sleep_start()；唤醒即重启，由 bsp_button_init() 重新初始化。
esp_err_t bsp_button_prepare_deep_sleep(int *level);
