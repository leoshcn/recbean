# 设计：录音豆 MVP

## 1. 架构与边界

```text
[AI Passport 设备]                 [测试机 192.168.2.144 /root/recbean, Docker Compose]        [豆包]
 按键/屏幕/ES8311                    recbean-server (Python, FastAPI)
  ── ws://<srv>:8090/device/ws ──►  ├─ 设备网关：鉴权、接收 PCM、回推转写      ── wss ──►  sauc/bigmodel_async
     16 kHz s16le mono 200ms 包     ├─ ASR 会话：豆包二进制协议客户端          ◄──────────  (speaker info)
  ◄── JSON 转写事件 ───────────────  ├─ 存储：SQLite + 音频文件（/data volume）
                                    └─ Web：REST API + 静态前端（同端口；容器 8080 → 宿主 8090）
                                         ▲
                              浏览器 http://192.168.2.144:8090
```

- 固件（`firmware/`）只负责采音、上传、显示、按键、配网；不持有豆包密钥，不做 TLS。
- 服务端（`server/`）是唯一与豆包通信的组件，负责持久化与 Web。
- 单容器单端口（容器内 8080，测试机宿主映射 8090，因宿主 8080 被其他服务占用）同时服务设备 WebSocket、REST API 与前端静态文件；不做 UDP 发现（服务器地址在配网页填写，默认 `192.168.2.144:8090`）。

## 2. 仓库布局（D5）

```text
recbean/
├─ firmware/          从 ai-passport main@0b9e4c8 导入的 BSP、构建、tools、tests；应用代码重写于 firmware/main/
│  └─ UPSTREAM.md     记录上游来源提交与导入范围
├─ server/
│  ├─ app/            FastAPI：device_ws.py, doubao_asr.py, store.py, api.py, models.py
│  ├─ web/            前端（Vue 3 + Vite，构建产物由 FastAPI 静态托管）
│  ├─ tests/          pytest：协议编解码、转写合并、API
│  └─ Dockerfile      多阶段：node 构建前端 → python 运行
├─ docker-compose.yml
└─ .env.example       DOUBAO_APP_KEY / DOUBAO_ACCESS_KEY / DOUBAO_RESOURCE_ID（真实 .env 不入库）
```

## 3. 设备 ↔ 服务协议（v1，可平滑升级到 wss）

WebSocket `ws://<host>:<port>/device/ws`，Header `Authorization: Bearer <device_token>`。

- 设备 → 服务
  - 文本 `{"type":"start","sample_rate":16000,"ch":1,"codec":"ima_adpcm","fw":"x.y.z"}`
  - 二进制：音频帧，每帧 200 ms = 3200 样本 → `seq`(uint32 LE) + `predictor`(int16 LE) + `step_index`(uint8) + 保留 1 B + 1600 B ADPCM 数据；每帧独立可解码，便于断线续传与去重
  - 文本 `{"type":"stop"}`
  - 文本 `{"type":"resume","recording_id":..,"next_seq":..}`（断线重连）
- 服务 → 设备
  - `{"type":"started","recording_id":"..."}`
  - `{"type":"partial","text":"..."}`（进行中句子，无说话人）
  - `{"type":"final","idx":n,"speaker":1,"start_ms":..,"end_ms":..,"text":"..."}`
  - `{"type":"error","code":"asr_error|auth|busy","msg":"..."}`
  - `{"type":"saved","recording_id":"...","duration_ms":..,"sentences":n}`
- 配对：`POST /device/pair {"device_id"}` → 返回 4 位码；Web 上确认后 `GET /device/pair/{id}` 拿到 token。设备令牌存 NVS。
- 设备端文本只发送定稿句和最新 partial（截断 ≤ 256 B），控制设备内存占用。

## 4. 豆包 ASR 接入

- 端点 `wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async`；Resource-Id 默认 `volc.seedasr.sauc.duration`（2.0 小时版，可由 env 覆盖）。
- 请求参数：`audio{format:pcm,rate:16000,bits:16,channel:1}`，`request{model_name:bigmodel,enable_punc:true,enable_itn:true,enable_speaker_info:true,ssd_version:"200",enable_nonstream:true,show_utterances:true}`；语言用默认中英文模型。
- 结果合并：`utterances` 中 `definite=true` 的句子 → 定稿入库并推送 `final`；最后一个未定稿 → `partial`。说话人字段名以真机返回为准（文档写在 additions 中），在 `doubao_asr.py` 中集中解析并有单测 fixture。
- 记录 `X-Tt-Logid` 到日志，便于排错。
- ASR 失败：录音继续落盘，recording.status=`asr_error`，通知设备。

## 5. 数据模型（SQLite）

- `recording(id, title, device_id, started_at, duration_ms, status[recording|done|asr_error|interrupted], audio_path)`
- `speaker(recording_id, idx, name)` —— 改名只改此表
- `sentence(id, recording_id, idx, speaker_idx, start_ms, end_ms, text, edited)`
- `device(id, token_hash, paired_at, name)`
- 音频：`/data/audio/<id>.wav`（服务端解码 ADPCM 后边收边写 PCM，结束时补 WAV 头）。

## 6. Web

- 页面：列表（倒序、时长、说话人数、首句、删除）、详情（句子列表 + 播放器，点句跳转，内联编辑文本，下拉改说话人，说话人改名，改标题）、导出 TXT、配对确认页。
- API：`GET/DELETE /api/recordings`、`GET/PATCH /api/recordings/{id}`、`PATCH /api/recordings/{id}/sentences/{sid}`、`PATCH /api/recordings/{id}/speakers/{idx}`、`GET /api/recordings/{id}/audio`（支持 Range）、`GET /api/recordings/{id}/export.txt`、`GET/POST /api/pairings`。
- 局域网第一版不做登录（D2）。

## 7. 固件设计要点

- 任务：`audio_task`（`bsp_audio_read` 200 ms → IMA-ADPCM 编码 → 环形缓冲）、`net_task`（esp_websocket_client 发送/接收）、UI 由 LVGL 定时器消费事件队列；按键回调只投递事件。
- 内存预算（无 PSRAM）：断网缓存为 ADPCM 环形缓冲约 80 KB（≈ 10 s，50 帧 × 1608 B），静态分配；若实测堆不足则按帧数下调并回报。见 R-1。转写显示只保留最近 N 句（如 30 句）文本，旧句释放。
- 字体：GB2312 16 px 子集字库置于 Flash（约 300 KB），按中文字体检查清单验证缺字。
- 状态机（待机/连接中/录音/确认结束/保存中/结束/设置/配网/配对）与协议解析、文本裁剪做成纯 C 逻辑，主机单测。
- 采样：`bsp_audio_set_format(16000,16,1)`；录音期间不播放。

## 8. 部署与运维

- `docker compose up -d --build` 于 `/root/recbean`；数据卷 `/root/recbean/data` 挂载 `/data`；`restart: unless-stopped`。
- 同步方式：本地 `scp`/`rsync` `server/`、`docker-compose.yml`、`.env` 到测试机；凭据仅存在测试机 `.env`。
- 回滚：保留上一版镜像 tag；数据卷独立于镜像。

## 9. 风险与取舍

- R-1 断网缓存（已决 D6）：原始 PCM 10 s = 320 KB 放不下，改为设备端 IMA-ADPCM 4:1，80 KB ≈ 10 s。剩余风险：识别质量轻微下降（AC10 验证）、80 KB 静态缓冲与 Wi-Fi/LVGL 争内存（先测堆再定帧数）。参考 `docs/reference/shinku-chen/audio-compression-trade-offs.zh_CN.md`。
- R-2 说话人分离仅在定稿句出现，且实时聚类可能前期编号不稳定；Web 改归说话人兜底。
- R-3 Wi-Fi + WebSocket + LVGL + 大字库同时运行的内存峰值，需真机测。
- R-4 豆包计费与并发：小时版按时长计费，测试期注意用量。
