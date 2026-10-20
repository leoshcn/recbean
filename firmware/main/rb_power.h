// main/rb_power.h —— 应用层电源编排：两级省电的执行端（10-20-power-battery-optimization）。
//
// 决策归 rb_app（纯逻辑：何时进/出浅睡、何时深睡及原因，见 rb_app.h 顶部说明）；
// 本模块只按动作位调用 BSP 原子能力与网络模块。全部接口只能在 app 任务中调用。
//
// 浅睡（可逆，RB_ACT_POWER_LIGHT_ENTER/EXIT）：
//   进入  背光已为 0 → 暂停 LVGL（停定时器并挂起 taskLVGL） →ST7789 Sleep In → 按键暂停（释放 ADC、GPIO0 低电平唤醒）
//         → Wi-Fi 低功耗并暂停服务器探测 → esp_pm 允许降频与自动 light sleep
//   退出  逆序恢复；屏幕先 Sleep Out 并强制全屏重绘，背光由 app 任务随后下发
//   按键唤醒：GPIO 中断 → RB_EV_WAKE_KEY 入队（rb_bus_event_from_isr）
// 深睡（终端，RB_ACT_POWER_DEEP）：见 rb_power_enter_deep()。
// 调试：编译时定义 RB_POWER_DEBUG_NO_SLEEP 可保留浅睡的其他步骤但不进自动 light sleep
// （USB-Serial-JTAG 日志在 light sleep 中会断开）。
#pragma once

#include "rb_app.h"

#include <stdbool.h>

// 启动时尽早调用：打印唤醒原因与上次入睡记录，并清除记录。仅当本次确实是深睡后被
// 按键唤醒时返回上次的入睡原因；冷启动/复位返回 RB_SLEEP_NONE。
rb_sleep_reason_t rb_power_boot(void);

void rb_power_light_enter(void);
void rb_power_light_exit(void);

// 每个 TICK 调用：重试未完成的按键暂停（入睡瞬间有键按住）或恢复（ADC 重建失败）。
void rb_power_poll(void);

// 浅睡步骤是否已执行（app 任务据此放慢节拍、跳过重绘）。
bool rb_power_light_active(void);

// 执行深睡（终端）。调用前 app 任务已执行其他动作并重绘（低电时屏幕上是“电量过低”）。
// 顺序：（低电时停留提示）→ 停 Wi-Fi → 持 LVGL 锁 → CW2017 sleep → ES8311 sleep →
// 释放 I2S → 释放 I2C → LCD Sleep In 并 hold → 按键脚释放 ADC、数字上拉、武装 GPIO 唤醒
// → esp_deep_sleep。还没关闭任何外设时就发现按键按着则返回 false（调用方投递
// RB_EV_DEEP_ABORT）；之后若按键仍按着则等待松开，卡键约 30 s 改为重启。正常不返回。
bool rb_power_enter_deep(rb_sleep_reason_t reason);
