# Implement: Web 浏览器独立录音

## 有序清单

1. **服务端重构（不改行为）**：`RecordingSession` 的帧解码和 resume 归属改为注入式，设备路径保持原样。跑 `pytest` 确认现有测试全部通过。
2. **服务端浏览器入口**：新增 `/web/ws`（PCM 帧解码、`resume_token`、`device_id="browser"`），在 `main.py` 注册。
3. **服务端测试**：在 `server/tests/` 中新增浏览器 ws 测试，覆盖以下场景：
   - start + 帧 + stop 端到端（WAV 时长、句子入库、标题和 kind）；
   - 非法 codec → `bad_request`，重复 start → `busy`；
   - resume：去重重发的帧；token 错误 → `resume_failed`；会话结束后 resume → `saved`；
   - 断线超时 → `interrupted`；
   - 帧长度错误被丢弃。
4. **前端采集模块**：`recorder.js` 与 AudioWorklet 负责混音、重采样、分帧、电平，以及两路源各自的开关。
5. **前端录音页**：`Record.vue` 包含状态机、ws 收发、环形缓冲与重连补传、实时转写、各类提示（R5）和 `beforeunload`；路由和导航加上“浏览器录音”。
6. **来源标签**：`RecordingList.vue` 和 `RecordingDetail.vue` 显示“浏览器”标签。
7. **文档**：根 `README.md` 补充群晖反代配置（含 WebSocket 标头）和使用说明；`server/README.md` 补充 `/web/ws` 协议摘要。
8. **构建并手工验收**：在 Windows Chrome/Edge 上通过群晖 HTTPS 地址逐条验证 AC1–AC5。

## 验证命令

```bash
cd server && .venv/Scripts/python -m pytest          # Linux: .venv/bin/python
cd server/web && npm run build
```

## 风险点 / 回滚点

- `server/app/device_ws.py` 的重构是唯一触及设备链路的改动，必须先单独完成并通过现有测试（第 1 步），再往下做。
- AudioWorklet 文件在 Vite 构建后的路径：确认 `npm run build` 产物中 worklet 能被正确加载。
- 手工验收依赖用户的群晖反代配置，以及一台带麦克风的 Windows 电脑。

## `task.py start` 前的检查

- `implement.jsonl` / `check.jsonl` 已填入真实的 spec 条目。
- 用户已批准最新的规划摘要。
