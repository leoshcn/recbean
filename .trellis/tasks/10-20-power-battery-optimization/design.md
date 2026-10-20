# 设计：两级省电与电量管理

## 1. 电源状态机

```
            按键/事件                       15 min 无操作 / 配网页 10 min / 低电
 ACTIVE ──120 s 无操作(待机类页面) / 待机页单击「下」──▶ LIGHT ─────────────────────────────▶ DEEP（终端，唤醒=重启）
   ▲                                   │
   └──────────── 任意按键 ◀────────────┘
 录音/连接/确认/保存：钉在 ACTIVE（R4）
```

- 状态归属：`rb_app`（纯逻辑，可主机单测）新增 `power` 字段与动作位：
  - `RB_ACT_POWER_LIGHT_ENTER` / `RB_ACT_POWER_LIGHT_EXIT` / `RB_ACT_POWER_DEEP`。
  - 判定输入：`view`、`last_activity_ms`、`backlight`、电量事件（新增 `RB_EV_BATTERY`，a=soc，c=mv）。
- 执行归属：`main.c:run_actions` 调新模块 `rb_power.c`（应用层电源编排），BSP 只提供原子能力。
- 常量放 `rb_app.h`（与现有 `RB_OFF_IDLE_MS` 同处）：`RB_DEEP_IDLE_MS 900000`、`RB_PROV_DEEP_MS 600000`、
  `RB_BATT_WARN_SOC 5`、`RB_BATT_CUT_SOC 2`、`RB_BATT_CUT_MV 3300`、`RB_BATT_SANE_MV 3600`。

## 2. 浅睡（LIGHT，可逆）

| 部件 | 进入 | 退出 |
|---|---|---|
| CPU | `CONFIG_PM_ENABLE=y`、`CONFIG_FREERTOS_USE_TICKLESS_IDLE=y`；启动时 `esp_pm_configure(max 160, min 40, light_sleep_enable=false)`，进入 LIGHT 时改为 `light_sleep_enable=true` | 改回 false |
| Wi-Fi | 保持 STA；`WIFI_PS_MAX_MODEM` + `listen_interval`（如 3~10 个 DTIM） | 恢复 `WIFI_PS_MIN_MODEM` |
| 服务器探测 | `rb_net` 新增 `rb_net_set_low_power(bool)`：LIGHT 时停止 15 s 探测；退出时立即探测一次 | — |
| LCD | 新 BSP 接口 `bsp_display_sleep(bool)`：发 DISPOFF+SLPIN / SLPOUT+DISPON（可逆，不 hold 引脚） | 先 SLPOUT 等 120 ms 再开背光 |
| LVGL | `lvgl_port_stop()` | `lvgl_port_resume()` + 强制全屏重绘 |
| 按键 | 新 BSP 接口 `bsp_button_suspend()`：`iot_button_stop()`、删除 ADC unit、GPIO0 切数字输入上拉、`gpio_wakeup_enable(LOW)` + `esp_sleep_enable_gpio_wakeup()`，GPIO 中断通知 app 任务 | `bsp_button_resume()`：关 GPIO 中断/唤醒、重建 ADC unit、`iot_button_resume()` |
| app/net 任务 | app 任务 TICK 在 LIGHT 时放慢到 1 s；net 任务 200 ms 轮询在 LIGHT 时改为 1 s | 恢复 |

关键约束：ADC 占用 GPIO0 时数字电平读回 0（参见 `docs/reference/shinku-chen/landscape-rotation-and-deep-sleep-key-wake.zh_CN.md`），
所以浅睡和深睡都必须先释放 ADC。三键分压 0/300/595 mV 均低于数字低电平阈值，任意键都能触发唤醒。

按键唤醒后的语义：GPIO 中断 → app 任务收到 `RB_EV_WAKE_KEY` → 退出 LIGHT → `bsp_button_resume()`。
若用户仍按住“上”，button 组件恢复后检测到按下 → `PTT_DOWN`，沿用现有 `ptt_down(was_off)` 规则；
单击“确定”在恢复前已松开时只亮屏，符合“首次按键只亮屏”。

### 手动进入浅睡（R10）

- `rb_app.c:handle_key` 的 `RB_VIEW_IDLE` 分支：`RB_EV_KEY_DOWN` → `app->manual_off = true`，背光目标立即为 0，返回 `RB_ACT_BACKLIGHT | RB_ACT_POWER_LIGHT_ENTER`；`last_activity_ms` 记为当前时刻，深睡倒计时从熄屏起算（与自动熄屏同一计时基准：`light_since_ms`）。
- `backlight_target()`：`manual_off` 为真时返回 0；任何按键/视图切换清除 `manual_off`。
- 熄屏状态下的首次「下」键仍只负责亮屏（沿用 `was_off` 规则），不会反复切换。
- CLICK 事件在松键后才发出，因此进入浅睡时 GPIO0 已为高电平；`bsp_button_suspend()` 仍回读电平，若为低则延迟 200 ms 重试，防止立即被唤醒。

## 3. 深睡（DEEP，终端）

顺序（`rb_power_enter_deep()`）：
1. app 层：取消会话（正常情况下不会在会话中触发；低电路径先走 STOP 等待 saved 或超时）。
2. 若 UI 需要显示“电量过低”，先渲染并亮屏 2 s。
3. `esp_wifi_disconnect/stop`。
4. 持 LVGL 锁：`bsp_battery_sleep()` → `bsp_audio_sleep()` → `bsp_audio_prepare_deep_sleep()` → `bsp_i2c_prepare_deep_sleep()` → `bsp_display_prepare_deep_sleep()`。
5. `bsp_button_prepare_deep_sleep()`（新增，移植自上游经验）：释放 ADC、上拉、回读电平；按住则返回“忙”，推迟 1 s 重试。
6. `esp_deep_sleep_enable_gpio_wakeup(1ULL<<0, LOW)`；RTC 内存记录入睡原因（idle / prov / low_batt）。
7. `esp_deep_sleep_start()`；若返回则 `esp_restart()`。

唤醒：`app_main` 读 `esp_sleep_get_wakeup_cause()` 与 RTC 记录并打日志；若原因为 low_batt 且读到电量仍低于阈值（R9），
只初始化显示和电量计，显示提示 2 s 后再次深睡，不启动 Wi-Fi。

注意：CW2017 sleep 会停止电量计算；唤醒后 `bsp_battery_init()` 走 `cw_enter_active()` 重启路径，正是 B1 场景，因此 R8 必须同时完成。

## 4. 电量读数修正（R8）

- `bsp_battery.c:cw_wait_soc_ready`：要求连续 3 次（100 ms 间隔）读到相同且 ≤100 的值；若为 0，再校验电压 < `RB_BATT_SANE_MV` 才接受，否则继续等，最长 5 s 后以 -1 返回（UI 隐藏电量而非显示 0）。
- 应用层 `main.c:refresh_info`：电量读取与渲染解耦——每个 TICK 检查是否到 60 s，到期读取并在数值变化时置 dirty（ACTIVE），LIGHT 下也读取以支撑低电保护，但不重绘。
- 合理性：`soc == 0 && mv > RB_BATT_SANE_MV` → 视为 -1；同时把读数以 `RB_EV_BATTERY` 送入状态机做低电判定（判定连续 2 次满足，避免瞬时压降误触发）。

## 5. 兼容与风险

| 风险 | 缓解 |
|---|---|
| 自动 light sleep 下 USB-Serial-JTAG 日志断开 | 预期行为；调试可编译期关闭（`RB_POWER_DEBUG_NO_SLEEP`）。真机验证用电流表而非日志判定 |
| light sleep 与 I2S/SPI DMA 冲突 | 仅在 LIGHT（无会话、LVGL 已停）时允许 light sleep；PM 锁由 esp-idf 驱动自身持有 |
| Wi-Fi 低功耗导致服务端连接延迟/探测失败 | 退出 LIGHT 立即探测；会议录音开始仍走现有连接守护超时 |
| LCD SLPOUT 后 120 ms 内显示残影 | 先 SLPOUT、等待、重绘，最后开背光 |
| 按键脚 GPIO 唤醒“睡下即醒” | suspend 前回读电平；按住则不入睡 |
| 内存：`CONFIG_PM_ENABLE` 增加少量 IRAM/DRAM | 构建后检查 `idf.py size` 与 `rb_mem` 日志 |

回滚：所有新行为由 `rb_power` 编排；把 `CONFIG_PM_ENABLE` 关掉、`RB_DEEP_IDLE_MS` 设为 0 即退回原行为。
