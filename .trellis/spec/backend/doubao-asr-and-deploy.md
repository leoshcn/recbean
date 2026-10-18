# Doubao ASR integration & test deployment

> Facts verified against the real service and test machine on 2026-10-18 (task `10-18-recbean-mvp`).

## Streaming ASR (`server/app/doubao_asr.py`)

- Endpoint `wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async`, resource `volc.seedasr.sauc.duration` (2.0 hourly).
- Auth: new-console **API Key only** → header `X-Api-Key` (env `DOUBAO_API_KEY`). Old-console `X-Api-App-Key` + `X-Api-Access-Key` still supported as fallback (`build_auth_headers`).
- Speaker diarization needs `enable_speaker_info=true` + `ssd_version="200"` + `enable_nonstream=true`.
- **Speaker label location (verified):** `result.utterances[].additions.speaker_id`, a string starting at `"0"`, only on `definite=true` utterances. Keep all parsing in `extract_speaker()`; the server renumbers raw labels to 1..N by first appearance.
- No threshold / speaker-count parameter exists for streaming diarization. Short utterances (< 1 s) and emotional / child voices get split into extra speakers; the Web "合并说话人" endpoint is the correction path.
- Always log `X-Tt-Logid` for every session (also on handshake failure).

## File ASR (not used yet — option B deferred)

- With the current key only 2.0 resources are granted: `volc.seedasr.auc` works; `volc.bigasr.auc` and `volc.bigasr.auc_turbo` return `45000030 requested resource not granted`.
- `submit/query` accepts inline `audio.data` (base64) even though the doc marks `audio.url` required — usable from a LAN-only server.
- `recognize/flash` with `volc.seedasr.auc` answers but diarization is ineffective (everything speaker 1).
- Raw docs: `.trellis/tasks/archive/*/10-18-recbean-mvp/research/doubao-file-asr-*.md`.

## Audio contract

- Device uploads IMA-ADPCM frames (design §3); server decodes to 16 kHz s16le PCM for Doubao and for the WAV on disk. PCM vs ADPCM measured CER 0 % / 0 % on the same clip — keep ADPCM.
- Shared vectors: `server/tests/fixtures/adpcm_vectors.json` ↔ `firmware/tests/fixtures/adpcm_vectors.json` must stay byte-identical (`firmware/tools/validate.sh` checks).

## Test machine deployment (192.168.2.144, `/root/recbean`)

- SSH with the local key works (`C:/Windows/System32/OpenSSH/ssh.exe` / `scp.exe`); no password needed.
- Host port **8090** → container 8080 (host 8080 belongs to another stack, `expense-gauge`; never touch it).
- Package with Windows `C:/Windows/System32/tar.exe` (brush's builtin `tar` lacks `--exclude`), excluding `.venv node_modules dist .pytest_cache __pycache__ .localdata`; never ship `.env` or `data/`.
- Before `docker compose up -d --build`, tag the running image `recbean:prev` for rollback. Data lives in `./data` (bind mount) and survives rebuilds.
- `.env` exists only on the test machine (chmod 600); the user writes the key themselves.
- Run credentialed probes inside the container (`docker compose exec -T recbean python ...`) so the key never leaves the machine.
