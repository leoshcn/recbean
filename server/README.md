# recbean server

FastAPI + SQLite. One port (8080 in the container, published as host 8090 by
`docker-compose.yml`) serves the device WebSocket, the REST API and the
built Vue 3 Web UI. Design: `.trellis/tasks/archive/2026-10/10-18-recbean-mvp/design.md` §3–§6.

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

Use the one-command script from the repo root (details in the root `README.md`):

```bash
./deploy.sh              # package, upload, keep recbean:prev, rebuild, health check
./deploy.sh --rollback   # back to recbean:prev
```

Smoke test against the deployed server:

```bash
python3 server/tools/fake_device.py --host 192.168.2.144:8090 --auto-confirm --wav two_speakers.wav
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
- `start` may carry `"mode":"meeting"|"note"`; missing or unknown → `meeting` (old
  firmware). The recording's `kind` is set from it and the default title is
  `会议 MM-DD HH:MM` / `笔记 MM-DD HH:MM`. A `note` runs ASR with speaker diarization off
  (`enable_speaker_info=false`, no `ssd_version`) and every `final` has `speaker` 1. All
  other messages are identical for both modes. Deploy the server before firmware that
  sends `mode` (an old server just stores notes as meetings).

## Web REST API (summary)

- `GET /api/recordings[?kind=meeting|note]` (list, newest first, with `kind` /
  `speaker_count` / `sentence_count` / `first_sentence` / `live`; bad kind → 422);
  `GET|PATCH {"title"}|DELETE /api/recordings/{id}` (DELETE → 409 while live).
  Existing databases get `recording.kind` added on startup (old rows → `meeting`).
- Notes are grouped per calendar day in `TZ_NAME` (default Asia/Shanghai):
  `GET /api/notes/days` → `[{date:"YYYY-MM-DD", count, duration_ms, first_text, live}]`
  (newest day first); `GET /api/notes/days/{date}` → `{date, segments:[{id, title,
  started_at, time:"HH:MM", duration_ms, status, error, live, sentences:[...]}]}` (oldest
  first); `GET /api/notes/days/{date}/export.txt` → `笔记 YYYY-MM-DD`, blank line, then one
  `HH:MM 文字` line per non-empty segment. Bad date → 422. Per-segment audio / edit /
  delete use the `/api/recordings/{id}/...` endpoints.
- Hotwords (shared by notes and meetings): `GET /api/hotwords` → `{budget, used,
  items:[{id, word, tokens, active}]}`; `POST {"words":[...]}` (items may contain several
  lines; blank lines ignored, duplicates skipped) → `{added, skipped, ...state}`;
  `PATCH /api/hotwords/{id} {"word"}` (409 duplicate); `DELETE /api/hotwords/{id}`;
  `PUT /api/hotwords/order {"ids":[...]}` (must be a permutation of all ids, else 400).
  Words are trimmed, 1–32 characters (422 otherwise). Mutations return the new state.
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

Hotwords (`app/hotwords.py`): every session gets the hotwords that fit
`HOTWORD_TOKEN_BUDGET` (default 100) taken from the top of the list; the first word that
does not fit and all words after it are inactive. Estimate per word: 1 token per CJK
character / other symbol, 1 per run of ASCII letters+digits, +1 separator. They are sent
as `request.corpus.context = json.dumps({"hotwords":[{"word":..}]}, ensure_ascii=False)`;
no hotwords → no `corpus` key. The list is snapshotted at `start`, so edits only affect
later recordings. Only the count is logged, never the words.

Real-API check (2026-10-18, `volc.seedasr.sauc.duration`, probe
`server/.localdata/hotword_probe.py` run inside the container): no error and no visible
truncation up to 10 001 words (~40 000 characters, 260 KB context) — a target word placed
first, in the middle or last was still applied. The documented "~100 tokens" is therefore
not enforced as a hard limit; the budget is our own conservative cap and can be raised.
