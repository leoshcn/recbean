# Implement Plan

1. `server/app/transcode.py`：`m4a_path` / `is_fresh` / `ensure_m4a` / `TranscodeError`（见 design.md）。
2. `server/app/device_ws.py` `RecordingSession.finish`：meeting 且 duration>0 时后台 ensure_m4a，异常记 error 日志；Gateway 持有任务集合。
3. `server/app/api.py`：新增 `GET /recordings/{rec_id}/audio.m4a`；`delete_recording` 同步删除 m4a。
4. `server/web/src/api.js` 加 `audioM4aUrl`；`RecordingDetail.vue` 加按钮（录音中禁用）。
5. `server/Dockerfile` runtime 阶段 `apt-get install -y --no-install-recommends ffmpeg && rm -rf /var/lib/apt/lists/*`。
6. 测试 `server/tests/test_api.py`（或新建 `test_transcode.py`）：生成+缓存、并发单次、409/404、删除清理、ffmpeg 缺失 500；无 ffmpeg 时真实转码用例 skip。
7. 文档：`README.md` 浏览器录音/使用处补一句；`server/README.md` 接口列表加 `audio.m4a`。
8. 必要时把“派生音频缓存 + ffmpeg”约定写入 `.trellis/spec/backend/`。

## 验证

- `cd server && .venv/Scripts/python -m pytest`
- `cd server/web && npm run build`
- 部署后在测试机：录一段会议 → `ffprobe data/audio/<id>.m4a`；浏览器下载验证。

## 风险点

- `finish` 是关键路径：后台任务必须吞掉全部异常，绝不能阻塞或改变 status。
- 群晖 NAS 上首次下载长录音的耗时；反代超时默认 60 s，若超时需提示用户稍后重试（收尾时已预生成，一般不会遇到）。
