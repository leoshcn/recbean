# recbean server

FastAPI + SQLite. One port (8080 in the container, published as host 8090 by
`docker-compose.yml`) serves the device WebSocket, the REST API and the
built Vue 3 Web UI. Design: `.trellis/tasks/10-18-recbean-mvp/design.md` §3–§6.

## Develop / test

```bash
cd server
py -3 -m venv .venv            # Linux: python3 -m venv .venv
.venv/Scripts/python -m pip install -r requirements-dev.txt
.venv/Scripts/python -m pytest
DATA_DIR=./.localdata .venv/Scripts/python -m uvicorn app.main:default_app --factory --port 8080

cd web && npm ci && npm run build   # output web/dist, served by the server
```

## Deploy (test machine)

```bash
cp .env.example .env    # fill DOUBAO_* on the server only
docker compose up -d --build
python3 server/tools/fake_device.py --host 127.0.0.1:8090 --auto-confirm --wav two_speakers.wav
```

## Device-facing contract (summary)

- Pairing: `POST /device/pair {"device_id":"..."}` → `{"code":"1234","expires_in":600}`.
  `GET /device/pair/{device_id}` → `{"status":"pending","code":"1234"}` until the code is
  confirmed on the Web page, then once `{"status":"paired","token":"..."}`; afterwards
  (or when unknown / expired) HTTP 404 `{"status":"unknown"}`.
- WebSocket `ws://host:port/device/ws` (container port 8080; published as 8090 on the
  test machine, device default `192.168.2.144:8090`), header `Authorization: Bearer <token>`
  (`?token=` query is accepted as a fallback). Bad token → `{"type":"error","code":"auth"}`
  then close 1008.
- Audio frame (binary, little-endian): `seq u32 | predictor i16 | step_index u8 | 0 u8 |
  1600 B IMA-ADPCM (3200 samples, low nibble first)`. Header holds the codec state *before*
  the first sample. Shared vectors: `tests/fixtures/adpcm_vectors.json`
  (regenerate with `tools/gen_adpcm_vectors.py`).
- `seq` starts at 0 per recording. Duplicates (`seq` < expected) are dropped; gaps are
  filled with silence so timestamps stay aligned.
- `resume` → `{"type":"started","recording_id":..,"resumed":true,"next_seq":N}` where N
  is the next seq the server expects; then buffered `final`/`error` events are replayed.
  If the recording was already finalized (resume timeout, default 30 s) the reply is
  its `saved` event; unknown id → `error` `resume_failed`.
- Extra error codes besides `asr_error|auth|busy`: `bad_request`, `not_recording`,
  `resume_failed`. `{"type":"ping"}` → `{"type":"pong"}`.
- `partial` text keeps the last ≤256 UTF-8 bytes, `final` text the first ≤256 bytes
  (full text is stored). `final.idx` is 0-based, `speaker` 1-based.

## Web REST API (summary)

- `GET /api/recordings` (list, newest first, with `speaker_count` / `sentence_count` /
  `first_sentence` / `live`); `GET|PATCH {"title"}|DELETE /api/recordings/{id}`
  (DELETE → 409 while live).
- `PATCH /api/recordings/{id}/sentences/{sid} {"text"?, "speaker_idx"?}` (text edit sets
  `edited`; reassignment does not).
- `PATCH /api/recordings/{id}/speakers/{idx} {"name"}` → speaker list.
- `POST /api/recordings/{id}/speakers/merge {"from": X, "into": Y}` → `{"moved": N,
  "speakers": [...]}`. In one transaction every sentence of X becomes Y (not marked
  `edited`), speaker X is deleted, Y keeps its name. 400 if X == Y, 404 for an unknown
  recording or speaker (neither a speaker row nor any sentence), 409 while live,
  422 for idx outside 1..99.
- `GET /api/recordings/{id}/audio` (WAV, Range supported), `GET /api/recordings/{id}/export.txt`.
- `GET|POST /api/pairings`, `GET /api/devices`, `DELETE /api/devices/{id}`, `GET /api/health`.

## Doubao ASR

`app/doubao_asr.py` implements the v3 sauc binary protocol. The speaker label was verified
against real responses (2026-10-18, seedasr 2.0): `utterances[].additions.speaker_id`, a
"0"-based string present only on definite utterances; the gateway maps labels to 1-based
indexes in order of first appearance. All speaker parsing lives in `extract_speaker()`
(fixture: `tests/fixtures/doubao_responses.json`); re-check with
`tools/asr_probe.py <16k wav>` if the upstream format changes.
`tools/asr_probe.py --compare --ref ref.txt` runs PCM vs ADPCM for AC10.
