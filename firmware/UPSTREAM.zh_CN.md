<p align="right">
  <strong>简体中文</strong> · <a href="UPSTREAM.md">English</a>
</p>

# 上游基线

录音豆（RecBean）固件派生自 FoloToy AI Passport 固件模板。

| 项目 | 内容 |
| --- | --- |
| 来源仓库 | FoloToy AI Passport 固件（本地检出 `ai-passport`，上游 `https://github.com/FoloToy/ai-passport`） |
| 分支 / 提交 | `main` 的 `0b9e4c81ee4421c0bac39ca3561d65a8285acd4a`（2026-09-28，“Merge pull request #91”） |
| 导入日期 | 2026-10-18 |
| 导入方式 | `git archive 0b9e4c8`（只取已提交的树；未复制本地工作区和其他分支） |
| 许可 | 见 [`LICENSE`](LICENSE)（未改动） |

## 原样导入

- `components/bsp/`（显示、LVGL 接入、按键、ES8311 音频、CW2017 电量计、共享 I2C）与 `partitions.csv`。
- `tools/`（`check_repo.py`、`verify_firmware.py`、`archive_firmware.py`、技能安装脚本），`tests/` 中的 BSP 主机测试与桩。
- `docs/`（开发、硬件设计、参考经验、贡献、品牌）、`assets/images/`、`skills/`、`.github/`、`AGENTS.md`、`CLAUDE.md`。
- `CMakeLists.txt`（保留项目名，产物仍叫 `FoloToy-AI-Passport*.bin`）。

`.github/workflows/` 保留用于仓库门禁（`actionlint`）；它们位于 RecBean 仓库的 `firmware/` 子目录中，GitHub 不会执行。

## 删除

- 硬件测试 demo 应用：`main/main.c`（菜单）、`main/demo_*.c/h`、`main/demo_navigation.*`、`main/ui_pixel*.c/h`。按强制 UI 重新设计规则，录音豆不得复用 demo 菜单和视觉外壳。
- demo 的主机测试与桩：`tests/test_ui_pixel_math.c`、`tests/test_demo_navigation.c`、`tests/test_demo_*_runtime.c`、`tests/demo_stubs/`。

## 修改

- `main/`：全新的录音豆应用（`main.c`、`rb_*.c/h`），新增 `main/idf_component.yml`（`espressif/esp_websocket_client`，LVGL 锁定 9.5.0）并更新 `dependencies.lock`。
- `sdkconfig.defaults`：关闭蓝牙，启用 Montserrat 28/40，显式 UTF-8 与缺字占位，LVGL 内存池 32 KB，放宽配网页 HTTP 头长度，以及 IRAM/堆预算选项（见下）。
- `tools/validate.sh`：运行录音豆主机测试（`tests/test_rb_*.c`、`tests/test_recbean_font.py`），替代已删除的 demo 测试；仓库中存在服务端时，校验 `tests/fixtures/adpcm_vectors.json` 与 `server/tests/fixtures/adpcm_vectors.json` 一致。
- `tools/check_repo.py`：允许根目录的 `UPSTREAM.md` / `UPSTREAM.zh_CN.md`。
- `tests/test_deep_sleep_contract.py`：删除读取 `main/demo_low_power.c` 的检查；BSP 深睡眠约束不变。
- `docs/development/engineering/coding-conventions*.md`：指向已删除 `main/ui_pixel.c` 的链接改为纯文本。
- 新增 `assets/fonts/recbean_font_16.c`、字符清单与 `tools/gen_recbean_font.py`（见 [`assets/README.zh_CN.md`](assets/README.zh_CN.md)）。

## 内部 RAM 预算

ESP32-C3 无 PSRAM，IRAM 与 DRAM 共用 SRAM。`idf.py size` 报告的静态占用：

| 构建 | 静态 D/IRAM 占用 | 留给堆 |
| --- | --- | --- |
| 上游 demo（`0b9e4c8`） | 191 386 B | 129 910 B |
| 录音豆首版（50 帧缓存，默认 IRAM 布局） | 270 422 B | 50 874 B |
| 录音豆交付版（40 帧缓存，下述 IRAM 选项） | 209 734 B | 111 562 B |

首版只剩 50 KB 堆，放不下 Wi-Fi、WebSocket 客户端、19.2 KB 的 LVGL 绘制缓冲、I2S DMA 缓冲和各任务栈。交付版把 Wi-Fi 热路径、堆函数、SPI master ISR 和日志移出 IRAM（约 37 KB），按 20 ms 分块读 PCM（`.bss` 少 6.4 KB），并通过 `main/CMakeLists.txt` 的 `RB_RING_FRAMES` 把断网 ADPCM 缓存从 50 帧（80.4 KB，10 s）**下调为 40 帧（64.3 KB，8 s）**。空闲堆与最大连续空闲块会以 `rb_mem` 标签打印：启动、Wi-Fi 初始化、Wi-Fi 连上、WebSocket 连接、录音开始、录音中每 10 s、会话结束。真机测得余量充足时，可改回 `RB_RING_FRAMES=50`。
