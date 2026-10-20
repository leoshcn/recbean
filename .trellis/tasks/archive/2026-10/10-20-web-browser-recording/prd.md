# Web 浏览器独立录音（麦克风+系统音频）

## Goal

在 Web 端增加一个不依赖 AI Passport 设备的录音入口：用户用 Windows 电脑上的 Chrome / Edge，同时录下**本机麦克风（自己的声音）**和**系统音频输出（Zoom、腾讯会议等对方的声音）**。录音与设备录音一样走豆包流式 ASR，实时显示转写并区分说话人，录完后进入现有的会议回放、编辑、导出流程。

## Background（代码与平台调研结论）

- 服务端 `RecordingSession`（`server/app/device_ws.py`）在 PCM 层已经和设备解耦：`_feed(pcm)` 同时写 WAV 和 ASR 队列（16 kHz s16le mono），ASR 接口（`server/app/asr.py`）只吃 PCM。与设备绑定的只有这几处：Bearer token 鉴权、`Gateway.start` 只接受 `ima_adpcm`、`on_frame` 中的 ADPCM 解码，以及 resume 时按 `device_id` 校验归属。
- `recording.device_id` 可为空，且没有外键（`server/app/store.py:15`）；列表 API 已经返回 `device_id`（`server/app/api.py:38`）。因此不需要改表就能区分来源。
- Web 端是 Vue 3 + vue-router 的 hash 路由（`server/web/src/main.js`），导航在 `server/web/src/App.vue`，没有额外依赖。`RecordingDetail.vue:240` 会轮询正在录制（live）的录音。
- 部署方式：Web 和设备 ws 共用 8090 端口，明文 HTTP，没有登录，只在局域网使用。
- 浏览器能力（Windows Chrome / Edge）：
  - 麦克风用 `getUserMedia`。
  - 系统音频用 `getDisplayMedia({video:true, audio:true, systemAudio:'include'})`：选择“整个屏幕”并勾选“共享系统音频”。这一步必须经过浏览器弹窗，且必须带 video 轨道，拿到后可立即停掉 video。选“标签页”则只录到该标签页的声音。Firefox 和 macOS 不支持整屏系统音频。
  - 两个 API 都只在安全上下文（HTTPS 或 localhost）中可用。
- 用外放时，麦克风会把对方的声音再录一遍，造成重复。缓解办法是提示用户戴耳机，并给麦克风开启 `echoCancellation`。

## Key Decisions

- **D1 HTTPS 由用户的群晖 DSM 反向代理提供**（2026-10-20，用户选 C）。群晖负责 TLS 终结并转发到 `http://192.168.2.144:8090`；服务端不处理证书，也不新开端口；设备仍走明文 `ws://…:8090`，固件不变。前端 ws 地址由 `location` 推导（`https:` 对应 `wss:`，同源）。群晖反代规则必须加上 WebSocket 自定义标头，这一点要写进 README。
- **D2 两路音源在浏览器内混成一路**（2026-10-20，用户选 A）。服务端只开一个豆包会话，走 `meeting` 模式做说话人分离，只存一个 WAV；“我”不会被自动标注，沿用现有的改名和合并功能。前端两路分开采集、发送前才混音，为以后改成分路识别留余地。
- **D3 断线后短时自动续录**（2026-10-20，用户选 A）。页面不停采集，断线期间的音频块暂存在内存里，每块带递增 `seq`；重连后发 `resume`，按服务端返回的 `next_seq` 补传。服务端沿用 resume 超时机制（`RESUME_TIMEOUT_S`，默认 30 s），超时后标记为 `interrupted` 并保留已录部分。刷新或关闭页面无法续录，录音期间用 `beforeunload` 提醒。

## Requirements

- **R1 录音页**：新增导航“浏览器录音”（路由 `/record`）。页面提供以下功能：
  - 勾选“麦克风”和“系统声音”（至少选一项），并可选择麦克风设备；
  - 开始和停止；
  - 显示录制时长和两路音量电平；
  - 显示连接状态（已连接 / 重连中 / 已中断）；
  - 实时显示转写：partial 行和 final 句子带说话人；
  - 停止后显示“已保存”，并给出跳转到 `/r/:id` 详情页的链接。
- **R2 采集与上传**：两路音频经 Web Audio 混音后，由 AudioWorklet 重采样为 16 kHz mono s16le，每 200 ms（3200 采样）作为一帧，带 `seq` 发往新的浏览器录音 WebSocket。
- **R3 服务端入口**：新增浏览器录音 WebSocket，复用 `RecordingSession` 的 WAV、ASR、热词、存储和 resume 逻辑。生成的录音为 `kind=meeting`，标题格式为 `会议 MM-DD HH:MM`，`device_id` 写入固定值 `browser`，用来标识来源。设备 ws 的协议和行为不变。
- **R4 续录（D3）**：断线后前端按指数退避自动重连，并携带 `recording_id` 和服务端在 `started` 中下发的 `resume_token`，然后补传缺失的帧。浏览器内的缓冲最多保留 60 s，超出时丢弃最旧的帧，服务端用静音补齐（现有 gap 逻辑）。
- **R5 环境与异常提示**：
  - 非安全上下文时，提示“请通过 HTTPS（群晖反代地址）访问”，并禁用开始按钮；
  - 浏览器不支持 `getDisplayMedia` 时，只允许录麦克风，并说明原因；
  - 共享时没有勾选系统音频（拿不到 audio 轨道）时，明确提示“未获取到系统声音，仅录制麦克风”，由用户决定继续还是重选；
  - 录音中用户点击浏览器的“停止共享”时，继续录麦克风并提示；
  - 勾选系统声音时提示“建议佩戴耳机”；
  - ASR 出错（`asr_error`）时提示，音频仍然保存。
- **R6 列表标识**：在会议列表和详情页中，`device_id == "browser"` 的录音显示“浏览器”来源标签。
- **R7 文档**：在根 `README.md` 和 `server/README.md` 中补充群晖反向代理配置说明（含 WebSocket 标头）、浏览器录音的使用说明，以及浏览器 ws 协议摘要。

## Acceptance Criteria

- [ ] AC1（R1/R2/R3）：在 Windows Chrome / Edge 中通过群晖 HTTPS 地址打开 `/record`，共享整个屏幕并带上系统音频，同时说话并播放 Zoom（或任意系统声音）。停止后，会议列表出现带“浏览器”标签的新录音，可以回放，转写同时包含两路内容。
- [ ] AC2（R1）：录制中，页面上能实时看到 partial 文本和带说话人编号的 final 句子；时长和电平在跳动。
- [ ] AC3（R4）：录制中断开网络 5–20 s 再恢复，页面自动重连，最终只生成一条录音，WAV 时长与实际录制时长相符（误差 ≤1 s），不出现重复音频。服务端测试覆盖了浏览器会话的 resume 去重和补帧。
- [ ] AC4（R4）：断线超过 `RESUME_TIMEOUT_S` 后，录音状态变为 `interrupted`，已录音频和转写保留，页面提示“录音已中断并保存”。
- [ ] AC5（R5）：用 `http://192.168.2.144:8090/#/record` 访问时显示 HTTPS 提示，开始按钮不可用；共享时不勾选系统音频，页面会提示“仅录制麦克风”。
- [ ] AC6（R3）：设备 ws 协议不变，`server/tests` 现有测试全部通过；新增的浏览器 ws 测试覆盖 start / 帧 / stop / resume / 错误 token / 非法 codec。
- [ ] AC7（R7）：README 中有群晖反代的配置步骤（含 WebSocket 自定义标头）。

## Out of Scope

- macOS、Linux、Firefox 上的系统音频采集；Safari 的适配。
- 分路识别、自动把麦克风标注为“我”（D2 的后续升级方向）。
- 刷新或关闭页面后的续录；离线录音后再上传。
- 接入 Zoom API 或云录制；把会议画面录成视频。
- 公网部署、登录鉴权；服务端自带 HTTPS 或证书管理。
- 笔记模式（note）的浏览器录音。
