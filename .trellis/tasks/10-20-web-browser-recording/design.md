# Design: Web 浏览器独立录音

## 1. 架构

```text
Windows Chrome/Edge (https://<群晖反代>/#/record)
  getUserMedia(mic, echoCancellation) ─┐
  getDisplayMedia(screen+systemAudio) ─┤→ AudioContext: 两路 GainNode → 混音
                                       │   → AudioWorklet: 重采样 16 kHz mono → Int16
                                       │   → 每 3200 采样一帧（200 ms），带 seq
                                       └→ WebSocket wss://<同源>/web/ws
群晖 DSM 反向代理（TLS 终结，WebSocket 标头）
  → http://192.168.2.144:8090  (recbean 容器 :8080)
      /web/ws → BrowserGateway → RecordingSession（复用：WAV / ASR / 热词 / 存储 / resume 超时）
```

## 2. 浏览器 WebSocket 协议（`/web/ws`）

与设备协议保持同一套消息语义，差异如下：

- **鉴权**：不使用设备 token（Web 本身就无登录，仅限局域网）。会话归属由 `resume_token` 保证：服务端在 `started` 中下发一个随机 token，`resume` 时必须带上。
- **start**：`{"type":"start","codec":"pcm_s16le","sample_rate":16000,"ch":1,"client":"web/<ver>"}`。其他 codec、采样率或声道数 → `error bad_request`。
- **started**：`{"type":"started","recording_id":"…","resume_token":"…"}`；续录时额外带 `"resumed":true,"next_seq":N`。
- **二进制帧**：`seq u32 LE | 6400 B s16le PCM（3200 采样）`。长度不对的帧记录警告后丢弃。
- **stop / ping / partial / final / error / saved**：与设备协议相同。浏览器端不截断文本也没关系，但服务端统一沿用现有的截断函数，保证行为一致。
- **resume**：`{"type":"resume","recording_id":"…","resume_token":"…","next_seq":N}`。token 不匹配或录音不存在 → `error resume_failed`；录音已经结束 → 返回其 `saved` 事件（与设备一致）。

## 3. 服务端改动

- `RecordingSession` 抽象出帧解码：构造时注入一个 `decode(buf) -> (seq, pcm)`。设备用 ADPCM 解码，浏览器用 `seq + raw PCM` 解码。gap 补静音、去重、时长更新逻辑保持一份。
- resume 归属校验：从“`device_id` 相等”改为一个可注入的归属判断。设备仍按 `device_id` 比较，浏览器按 `secrets.compare_digest(resume_token)` 比较。
- 新建 `BrowserGateway`（可以放在新模块 `app/web_ws.py`，也可以把 `Gateway` 参数化，以实际代码量最小者为准）：
  - 不做 token 鉴权；
  - `device_id="browser"`；
  - 浏览器会话可以并发，一个 ws 连接同时只能有一个录音，重复 `start` → `busy`。
- `main.py` 注册 `@app.websocket("/web/ws")`，挂载位置在 StaticFiles 之前。
- `recover_unfinished` 和 `shutdown` 已经按 `sessions` / `status` 处理，浏览器会话不需要额外改动。

## 4. 前端改动

- 新文件：
  - `server/web/src/views/Record.vue`：页面与状态机 `idle → acquiring → connecting → recording ⇄ reconnecting → stopping → saved | interrupted`。
  - `server/web/src/recorder.js`：采集、混音、AudioWorklet、分帧，与 Vue 无关。
  - `server/web/public/pcm-worklet.js`（或用 Vite 的 `?url` 引入）：AudioWorklet 处理器。
- 重采样：`AudioContext` 用设备的默认采样率（48 kHz 居多），在 worklet 里做线性插值和简单低通降到 16 kHz。不强制 `new AudioContext({sampleRate:16000})`，因为 Chrome 在这个设置下混入不同采样率的源时有兼容风险；实现时可以先试一下，能用就简化。
- 续录缓冲：环形缓冲最多保留 60 s 的帧（300 帧 × 6.4 KB ≈ 1.9 MB）。收到 `started(resumed, next_seq)` 后，从 `next_seq` 开始补发缓冲里的帧，再接上实时帧。重连采用指数退避（1 s 起，上限 5 s），直到服务端返回 `saved` 或 `resume_failed`。
- 系统声音轨道 `ended`（用户点击“停止共享”）时，断开该路的 GainNode，继续录麦克风并提示。拿到屏幕共享后立即 `videoTrack.stop()`。
- 在 `App.vue` 导航里新增“浏览器录音”；`RecordingList.vue` 和 `RecordingDetail.vue` 在 `device_id === 'browser'` 时显示“浏览器”标签。
- ws URL：`(location.protocol === 'https:' ? 'wss:' : 'ws:') + '//' + location.host + '/web/ws'`。

## 5. 兼容性与风险

| 风险 | 处理 |
|---|---|
| 群晖反代未开 WebSocket 标头 | README 写明配置；页面连接失败时提示检查反代的 WebSocket 设置 |
| 用户没勾选系统音频 | 检查 `getAudioTracks().length`，明确提示 |
| 外放导致回声/重复 | 麦克风开启 `echoCancellation/noiseSuppression`，并提示戴耳机 |
| 标签页在后台 | AudioWorklet 不受定时器节流影响；ws 发送也在音频回调里驱动 |
| 豆包会话时长 | 与设备会议相同，不新增风险 |
| 设备链路回归 | 帧解码和归属判断改为注入式，现有 `test_gateway.py` 必须全部通过 |

## 6. 回滚

纯新增入口：去掉 `/web/ws` 路由和前端导航即可回滚，数据库不变。部署时仍可用 `./deploy.sh --rollback`。
