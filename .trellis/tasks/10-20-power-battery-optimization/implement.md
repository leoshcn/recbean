# 实施计划

## 顺序（每步可独立构建）

1. **电量读数修正（R8，风险最低先做）**
   - `components/bsp/src/bsp_battery.c`：`cw_wait_soc_ready` 稳定性判定 + 0% 电压校验。
   - `main/main.c`：电量 60 s 定时读取与渲染解耦；合理性过滤；新增 `RB_EV_BATTERY` 投递。
2. **BSP 可逆接口**
   - `bsp_display_sleep(bool)`（SLPIN/SLPOUT，可逆）。
   - `bsp_button_suspend()/bsp_button_resume()`（释放/重建 ADC，GPIO 唤醒中断）、`bsp_button_prepare_deep_sleep()`。
   - 头文件注释说明可逆 / 终端语义；更新 `docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE*.md` 相应章节。
3. **状态机（纯逻辑）**
   - `rb_app.h/.c`：`power` 状态、常量、动作位 `RB_ACT_POWER_*`、`RB_EV_BATTERY` / `RB_EV_WAKE_KEY` 处理、低电判定与录音中先保存。
   - 待机页「下」键手动浅睡（`manual_off`，R10）。
   - `tests/test_rb_app.c`：覆盖 ACTIVE→LIGHT→DEEP 计时、会话中钉住、配网页 10 min、低电两次确认、唤醒按键语义、待机页「下」键进浅睡且其他页面「下」键不变。
4. **电源编排 `main/rb_power.c/.h`**
   - LIGHT 进入/退出（PM 配置、Wi-Fi PS、LVGL stop/resume、LCD sleep、按键 suspend/resume、任务节拍）。
   - DEEP 终端序列 + RTC 入睡原因 + 唤醒原因日志 + 低电唤醒再睡（R9）。
   - `rb_net.c`：`rb_net_set_low_power()`、`rb_net_stop()`；探测暂停。
   - `sdkconfig.defaults`：`CONFIG_PM_ENABLE=y`、`CONFIG_FREERTOS_USE_TICKLESS_IDLE=y`（附中文注释说明原因）。
5. **UI**：`rb_ui.c:217` 待机页提示改为「下 休眠 · 长按 确定 设置」；低电提示文案（≤5% 提示、“电量过低，即将休眠”），确认字体子集含新增汉字（`assets/README.md` 流程）。
6. **静态契约**：`tests/test_deep_sleep_contract.py` 增加应用层深睡顺序断言。

## 验证命令

```bash
cd firmware
idf.py build                      # 无新 warning；检查 idf.py size 内存余量
python tests/test_deep_sleep_contract.py
python tools/check_repo.py
tools/validate.sh                 # 主机单测（test_rb_app.c 等）
```

真机（需用户授权烧录，见 passport-device-test）：串联电流表测 ACTIVE / LIGHT / DEEP 三态电流，逐条执行 PRD AC1–AC9。

## 风险文件 / 回滚点

- `sdkconfig.defaults`：PM 开关影响全局时序；注意生成的 `sdkconfig` 已存在时需同步（见 spec 经验）。
- `bsp_button.c`：ADC 释放/重建与 iot_button 定时器的先后顺序（先 stop 再删 ADC，先建 ADC 再 resume）。
- `rb_net.c`：Wi-Fi 省电模式与重连逻辑。
- 每步完成后可单独回退；第 1 步可单独交付。

## start 前检查

- [ ] PRD 已获用户最终确认
- [ ] implement.jsonl / check.jsonl 已填入真实 spec 条目
