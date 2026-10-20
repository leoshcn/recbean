# Derived Audio (m4a download)

The WAV at `audio_dir/{rec_id}.wav` is the **only source of truth** for audio. Anything else (currently `{rec_id}.m4a`) is a disposable cache derived from it.

## Rules

- Never record into a compressed container. m4a/mp4 need the `moov` index written at the end, which breaks resume (`WavWriter` append) and crash repair (`repair_wav`). Encode only after `RecordingSession.finish`.
- All ffmpeg calls go through `server/app/transcode.py` (`ensure_m4a`). Do not spawn ffmpeg elsewhere.
- Freshness = m4a mtime ≥ WAV mtime. No DB column; deleting the m4a is always safe, the next download rebuilds it.
- `ensure_m4a` serialises per WAV path (refcounted `asyncio.Lock`), writes `<id>.m4a.tmp` then `os.replace`. The tmp extension hides the format, so `-f mp4` is required.
- Every failure (ffmpeg missing, cannot start, non-zero exit, `os.replace` error) must surface as `TranscodeError`; the API maps it to 500 + error log. Any other exception type escapes the handler and loses the log.
- Background transcoding after `finish` is fire-and-forget via `Gateway.schedule_m4a`: it runs only for `kind == "meeting"` with `duration > 0`, swallows everything except `CancelledError`, and must never change the recording's status. Task refs live in `Gateway.m4a_tasks` (shared with `BrowserGateway`) and are cancelled in `shutdown()`.
- If the recording is deleted while a transcode runs, remove the freshly written m4a (both the background path and the download endpoint do this).
- `delete_recording` unlinks both WAV and m4a.
- Encoding: AAC-LC, mono, 16 kHz, 32 kbps, `+faststart` (~14 MB/hour, ~1/8 of WAV; verified 2026-10-20 on the test machine: 38.8 s → 161 KB).

## Endpoint

`GET /api/recordings/{id}/audio.m4a` — 404 unknown / non-meeting / no or empty WAV, 409 live, 500 transcode failed, else `audio/mp4` attachment named like `export.txt` (`_attachment(rec, ext)`). Playback (`/audio`) stays WAV with Range support.

## Deployment & tests

- The runtime image installs `ffmpeg` via apt (`server/Dockerfile`). Locally on Windows, ffmpeg comes from WinGet.
- Tests needing real ffmpeg skip when `shutil.which("ffmpeg")` is None; everything else mocks `_ffmpeg`.
