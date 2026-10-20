// components/bsp/include/bsp_battery.h
// CellWise CW2017 电量计:I2C 0x63,与 ES8311 共用总线。
// BSP 写入自定义电池 profile,芯片直接给 SOC%,无需外部分压电阻与查表。
#pragma once

#include "esp_err.h"

// 初始化。内部会调 bsp_i2c_init()(幂等)。
// 芯片不应答时返回 ESP_ERR_NOT_FOUND —— 上层可据此在 UI 上标记该项不可用。
// 初始化会等待 SOC 就绪（连续 3 次相同且 ≤100；读到 0 时还要电压 < 3.6 V 才接受），
// 最多 5 秒；超时仍返回 ESP_OK 并保留设备，此时早期读数可能是 0，上层应结合电压过滤。
esp_err_t bsp_battery_init(void);

// 将 CW2017 CONFIG 写为睡眠值并回读确认，失败时重试一次。这是 deep
// sleep 前的终端操作；芯片未初始化时视为无需处理并返回成功。
esp_err_t bsp_battery_sleep(void);

// 剩余电量百分比 0..100;读失败返回 -1。电量计刚重启（写 profile / 深睡唤醒）时
// 可能短暂返回 0：电压正常（> 3.6 V）而 SOC=0 的读数应由上层视为无效。
int bsp_battery_soc(void);

// 电池电压 mV;读失败返回 -1。
int bsp_battery_mv(void);
