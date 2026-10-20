# Design: 会议录音 m4a 下载

## 边界

- 新模块 `server/app/transcode.py`：唯一负责调用 ffmpeg。
  - `m4a_path(wav: Path) -> Path`：同目录同名 `.m4a`。
  - `is_fresh(wav, m4a) -> bool`：m4a 存在且 mtime ≥ wav mtime。
  - `async ensure_m4a(wav: Path) -> Path`：不新鲜则转码；按路径持有 `asyncio.Lock`，同一录音并发只转一次；失败抛 `TranscodeError`。
  - 实现：`asyncio.create_subprocess_exec("ffmpeg", "-nostdin", "-y", "-loglevel", "error", "-i", wav, "-vn", "-ac", "1", "-ar", "16000", "-c:a", "aac", "-b:a", "32k", "-movflags", "+faststart", tmp)`，成功后 `os.replace(tmp, m4a)`；失败删 tmp，stderr 尾部写入异常信息。`shutil.which("ffmpeg")` 为空直接抛 `TranscodeError`。
- `RecordingSession.finish`（`device_ws.py`）：写完 status 后，若 `kind == "meeting"` 且 `duration > 0`，`asyncio.create_task(...)` 后台转码，异常只记日志（error），不影响返回值。任务引用保存在 Gateway 的集合里防止被 GC。
- `api.py`：
  - 新路由 `GET /recordings/{rec_id}/audio.m4a`（async）：校验 meeting / 非进行中 / WAV 存在且非空 → `await ensure_m4a` → `FileResponse(media_type="audio/mp4", Content-Disposition 同 export_txt 规则)`。
  - `delete_recording` 额外 unlink m4a。
- 前端：`api.js` 增加 `audioM4aUrl(id)`；`RecordingDetail.vue` 在导出 TXT 旁加 `<a class="btn">下载音频 (M4A)</a>`，`rec.live` 时禁用（沿用现有按钮/禁用样式）。

## 数据流

录音中：帧 → WAV（不变） → 收尾 `wav.close()` → status 写库 → 后台 ensure_m4a。
下载：请求 → ensure_m4a（新鲜则跳过）→ 返回文件。

## 兼容与迁移

- 无 DB 变更；m4a 是 WAV 的派生缓存，可随时删除重建。
- 历史录音首次下载时生成（1 小时录音在 NAS 上转码预计十余秒，可接受）。
- 回滚：`./deploy.sh --rollback`；遗留 m4a 文件无害。

## Trade-offs

- 选 ffmpeg 子进程而非 PyAV：镜像体积增加约 数十 MB，但实现最简单、AAC 编码器成熟。
- 用 mtime 判断新鲜度而不是 DB 字段：零迁移；续录后 WAV 更新会自动触发重转。
