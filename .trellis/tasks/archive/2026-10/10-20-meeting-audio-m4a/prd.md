# 录音压缩为 m4a 下载

## Goal

会议录音下载后要上传给其他 AI 做识别。当前录音只有未压缩 WAV（≈115 MB/小时），上传慢、易超限。目标：会议录音可下载为体积约 1/8 的 m4a（AAC），识别质量不受影响。

## Background（代码证据）

- 录音统一在服务端落盘为 16 kHz / 16-bit / 单声道 PCM WAV：`server/app/wav.py`（`WavWriter`），路径 `audio_dir/{rec_id}.wav`（`server/app/device_ws.py:140`、`:370`）；设备（IMA-ADPCM）与浏览器（PCM）录音都写入同一格式。32 KB/s ≈ 115 MB/小时。
- WAV 支撑断线续录（`WavWriter.__init__` 追加打开）与崩溃修复（`repair_wav`），因此录音过程中不能改写 m4a（m4a 的 moov 索引需在结束时写入）。
- 收尾位置：`RecordingSession.finish`（`device_ws.py:276-284`，`wav.close()` 后写 status `done`/`interrupted`/`asr_error`）；浏览器录音经 `web_ws.BrowserGateway` 复用同一收尾。
- 播放接口 `GET /api/recordings/{id}/audio`（`server/app/api.py:134`，Range，`audio/wav`）；删除接口 `api.py:77-89` 只删 WAV。
- 导出 TXT 的文件名模式：`api.py:196-205`（`Content-Disposition` 带 `filename*=UTF-8''`）。
- 会议详情页只有「导出 TXT」按钮（`server/web/src/views/RecordingDetail.vue:20`，URL 来自 `api.js:44 exportUrl`），没有音频下载入口。
- Docker 镜像 `python:3.12-slim` 未安装 ffmpeg（`server/Dockerfile`）。
- 录音 kind：`meeting` / `note`（`server/app/store.py:21`）。

## Decisions

- D1（用户确认）：**保留 WAV**。m4a 仅用于下载；播放、Range 拖动、续录、崩溃修复、ASR 均不改动。
- D2（用户确认）：**只覆盖会议（kind=meeting）**，笔记页不加入口。
- D3（技术）：录音收尾后在后台生成 m4a；下载时若 m4a 不存在则当场生成并缓存。历史录音、转码失败的录音由此自动兜底，无需批量补转脚本。
- D4（技术）：编码参数 AAC-LC、单声道、16 kHz、32 kbps（≈14 MB/小时）；使用系统 ffmpeg（Docker 镜像中 apt 安装），不引入 Python 编码库。

## Requirements

- R1 会议录音结束（`done` / `interrupted` / `asr_error`，只要 WAV 有音频）后，服务端在后台生成 `audio_dir/{rec_id}.m4a`，不阻塞收尾流程与设备回包。
- R2 新接口 `GET /api/recordings/{id}/audio.m4a`：返回 m4a 附件下载，文件名为录音标题（非法字符替换为 `_`，与导出 TXT 一致）+ `.m4a`。
  - m4a 不存在或比 WAV 旧时，当场转码后返回；同一录音的并发请求只转码一次。
  - 录音进行中 → 409；非 meeting → 404；无 WAV / 空录音 → 404；ffmpeg 缺失或转码失败 → 500 并记 error 日志，WAV 不受影响。
  - 转码先写临时文件再原子改名，避免下载到半成品。
- R3 删除录音时同时删除 m4a。
- R4 会议详情页在「导出 TXT」旁增加「下载音频 (M4A)」按钮；录音中禁用。
- R5 Dockerfile 安装 ffmpeg；README / server README 说明该功能。

## Acceptance Criteria

- AC1 录完一段会议后，`data/audio/` 下出现同名 `.m4a`，体积约为 WAV 的 1/8（32 kbps），ffprobe 显示 AAC、单声道、16 kHz，时长与 WAV 相差 < 1 s。（R1）
- AC2 详情页点击「下载音频 (M4A)」得到以录音标题命名的 .m4a，可在常见播放器播放。（R2、R4）
- AC3 手动删掉某录音的 .m4a 后再下载，仍能拿到文件，且磁盘上重新出现 .m4a。（R2、D3）
- AC4 录音中请求下载返回 409、按钮禁用；笔记录音请求返回 404。（R2、R4、D2）
- AC5 删除录音后 WAV 与 m4a 都被删除。（R3）
- AC6 页面播放、拖动进度条行为与改动前一致（仍为 WAV）。（D1）
- AC7 `pytest` 通过，新增用例覆盖：下载生成与缓存、409/404、删除清理、ffmpeg 不可用时 500（ffmpeg 不存在的环境下相关用例自动 skip 或用 mock）。
- AC8 部署到测试机后 `/api/health` 正常，AC1、AC2 实测通过。

## Out of Scope

- 固件改动（设备上行格式不变）；笔记录音下载；删除/替换 WAV；播放改用 m4a；可配置码率或其他格式（mp3/opus）；批量下载。
