# 实施计划：录音豆 MVP

## 0. 前置（开始实现前需用户提供）

- [ ] 豆包控制台凭据：新版控制台 API Key（`DOUBAO_API_KEY` → `X-Api-Key`，用户选定 2026-10-18；旧版 App Key + Access Key 仍兼容），已开通"豆包流式语音识别模型 2.0 小时版"。只写入测试机 `/root/recbean/.env`，不入库。
- [x] 确认测试机已装 Docker / Compose（2026-10-18 只读检查：主机名 haiman，Docker 29.9.0，Compose v5.6.0，根分区剩余 7.2 G，`/root/recbean` 尚不存在；本机 `~/.ssh` 密钥可免密登录 root@192.168.2.144，ssh 客户端为 `C:/Windows/System32/OpenSSH/ssh.exe`/`scp.exe`）。

## 1. 服务端（可先于固件独立完成）

1. [ ] `server/` 脚手架：FastAPI、SQLite、配置读取 env、日志。
2. [ ] `adpcm.py`：IMA-ADPCM 帧解码（与固件编码器共用测试向量）。
3. [ ] `doubao_asr.py`：豆包 v3 二进制帧编解码（header/序号/gzip/JSON），full client request、音频包、负包结束；解析 utterances → partial/final（含说话人）。用文档示例做单测 fixture。
3b. [ ] 用本地 16 kHz WAV 文件跑通真实豆包识别脚本 `server/tools/asr_probe.py`，确认说话人字段实际位置，更新解析与 fixture；同时跑 PCM vs ADPCM 识别对比（AC10）。
4. [ ] 设备网关 `device_ws.py`：token 鉴权、start/ADPCM 音频帧/stop/resume、解码落盘 WAV、转发豆包、回推事件、ASR 失败降级。
5. [ ] 配对 API + 设备表。
6. [ ] REST API（列表、详情、编辑句子/说话人/标题、删除、音频 Range、导出 TXT）。
7. [ ] `server/tools/fake_device.py`：用 WAV 模拟设备推流，端到端验证（不依赖硬件）。
8. [ ] 前端 Vue 3：列表、详情（播放器 + 点句跳转 + 内联编辑 + 改说话人/改名/改标题）、导出、配对确认页。
9. [ ] Dockerfile（多阶段）+ `docker-compose.yml` + `.env.example`。
10. [x] 部署到 192.168.2.144:/root/recbean，`docker compose up -d --build`，用 fake_device 在测试机上回归。（2026-10-18：已部署，宿主端口 8090；fake_device 配对/推流/断线 2 s 续传/保存通过，无凭据时正确标记 asr_error；测试数据已清理。真实豆包识别待凭据。）

进展（2026-10-18）：服务端 1–9 完成（pytest 47 passed）；3b 完成——真实豆包（seedasr 2.0，API Key）识别 TTS 合成两人对话 6/6 句文本与说话人全对；说话人字段确认为 `utterances[].additions.speaker_id`（"0" 起）；AC10 PCM/ADPCM CER 均 0%，说话人划分一致。

## 2. 固件

进展（2026-10-18）：1–9 完成（9：真机烧录经用户逐次同意，镜像 ca195623…764d；首版 86702d8a… 开机后 LVGL 内置池耗尽触发 task_wdt，改 CONFIG_LV_USE_CLIB_MALLOC 修复）；1–8 完成，Build PASS，主机测试通过（上游既有的 test_check_repo.py Windows 路径失败除外）；断网缓存因内存降为 40 帧 ≈ 8 s（`RB_RING_FRAMES`，真机测堆后可回调 50）。固件：`firmware/build/FoloToy-AI-Passport-full.bin` sha256 a23db130…e03。真机未测。


1. [ ] 导入 ai-passport `main@0b9e4c8` 到 `firmware/`（BSP、tools、tests、构建配置），写 `firmware/UPSTREAM.md`；移除演示菜单入口，确认 `tools/validate.sh` 通过。
2. [ ] 纯逻辑模块 + 主机单测：IMA-ADPCM 编码器（与服务端共用测试向量）、录音状态机、协议 JSON 解析、转写显示缓冲（保留最近 N 句、按像素宽度换行）、音频帧序号/重传缓冲（约 80 KB ≈ 10 s）。
3. [ ] 先测内存：Wi-Fi + WebSocket + LVGL + 字库启动后的空闲堆/最大连续块，确定断网缓存大小（见 design R-1）。
4. [ ] 配网（热点 `RecBean-Setup`，Wi-Fi + 服务器地址）与配对（4 位码），参考 `feature/xhs-dashboard` 的 xhs_net/xhs_store。
5. [ ] 录音链路：audio_task → 环形缓冲 → WebSocket 发送；断线重连 + resume。
6. [ ] UI：待机/录音/确认结束/结束/设置/配网/配对页面（全新设计，不使用 ui_pixel 演示外壳），说话人着色，电量右上。
7. [ ] GB2312 16 px 字库生成与集成，按中文字体检查清单验证。
8. [ ] `tools/validate.sh` 全量通过；产出固件包（passport-build）。
9. [ ] 征得同意后真机烧录测试（passport-device-test）。

## 3. 联调验收

- [ ] 真机：按确定开始 → 两人对话 ≥ 2 分钟 → 屏幕实时显示并区分说话人 → 二次确认结束 → Web 列表出现该录音，可播放、编辑、导出。
- [ ] 断网演练：录音中关路由器 ≤ 缓存时长后恢复，录音连续；超时则录音被结束且已传部分可见。
- [ ] 服务器停掉时开始录音 → 屏幕提示服务器未连接。

## 验证命令

- 服务端：`cd server && pytest`；`python tools/fake_device.py --wav samples/two_speakers.wav --host 192.168.2.144:8090 --auto-confirm`
- 固件：`cd firmware && ./tools/validate.sh`；`idf.py build`
- 部署：`ssh root@192.168.2.144 'cd /root/recbean && docker compose ps && docker compose logs --tail=50'`

## 风险点 / 回滚

- 固件导入是大批量文件，单独提交，便于回滚。
- 内存相关改动（缓存、字库、TLS 以后）每步记录空闲堆。
- 部署回滚：`docker compose` 切回上一镜像 tag；数据卷不随镜像删除。

## start 前检查

- [ ] implement.jsonl / check.jsonl 有真实条目
- [ ] 用户已批准最终规划摘要
- [ ] 凭据已就绪（可在服务端第 1~2 步之后再提供，但第 3 步前必须有）

## 收尾状态（2026-10-18）

- 真机已验证：配网、配对、录音上传、保存、实时转写文本（单人与亲子双人对话），堆内存稳定（录音中约 52 KB 空闲）。
- 真机发现：亲子对话被流式 SSD 拆为 4 人 → 新增 Web 合并说话人（D7/AC11，已部署）；方案 B 暂缓（见 prd D7）。
- 未验证（留待后续任务）：AC1 屏幕显示细节（说话人着色、3 s 首字）由用户目测、AC5 真机断网演练与服务器停机提示、AC8 中文缺字全面检查、AC11 浏览器内实际合并操作、ring 缓冲能否回调到 50 帧、i2s_channel_disable 噪声日志清理。
