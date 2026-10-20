# Browser Recording & Audio Ingress Gateways

Learned in task `10-20-web-browser-recording` (verified end-to-end through the Synology
reverse proxy on 2026-10-20). Wire protocol details live in `server/README.md`
("Browser recording WebSocket"); this file records the rules for changing the code.

## Adding an audio source = subclass `Gateway`

`server/app/device_ws.py` owns one copy of the session logic (WAV, ASR, hotwords, seq
dedupe / gap-silence, resume timeout, finish). A new ingress (browser, file, …) must
subclass `Gateway` and override only the hooks, never fork `RecordingSession`:

| Hook | Device | Browser (`app/web_ws.py`) |
|---|---|---|
| `authenticate(ws)` | Bearer token → device id | accept, `"browser"` |
| `check_format(msg)` | `ima_adpcm` 16k mono | `pcm_s16le` 16k mono |
| `decode(buf) -> (seq, pcm)` | ADPCM frame | `seq u32 LE` + 6400 B PCM |
| `start_kind(msg)` | `mode` → meeting/note | always meeting |
| `before_start(conn)` | finish this device's old recording | no-op (concurrent) |
| `started_extra(session)` | — | `resume_token` |
| `owns(rec_id, owner, conn, msg)` | `device_id` equal | `compare_digest(resume_token)` |

- All gateways **share one `sessions` dict** (`main.py`), otherwise `live`, DELETE-409
  and shutdown miss the other source's recordings.
- `device_id` doubles as the source tag (`"browser"`); no schema change. The Web reads it
  via `BROWSER_DEVICE` in `web/src/util.js`.
- Ownership checks must keep sources apart: a device can never resume a browser
  recording and vice versa (tested in `tests/test_web_ws.py`).

## Resume rule (bug fixed in this task)

In the shared ws handler, a **failed** `resume` (→ `resume_failed` or `saved`) must not
replace the connection's current `session`. Only assign when `resume()` returns a session.
Dropping it left the live recording undetached forever (`test_failed_resume_keeps_active_recording`
in both gateway test files).

## Browser capture constraints

- `getUserMedia` / `getDisplayMedia` exist only in a secure context. The server stays
  plain HTTP; HTTPS comes from the Synology DSM reverse proxy, whose rule **must** have the
  WebSocket custom header. Derive the ws URL from `location` (`https:` → `wss:`), never
  hard-code host/port/scheme.
- System audio on Windows Chrome/Edge: `getDisplayMedia({video:true, audio:true,
  systemAudio:'include'})`, user picks "entire screen" + "share system audio"; stop the
  video track immediately. No audio track = user did not tick the box → tell them.
- Mic with `echoCancellation`; still recommend headphones (speakers make the mic re-record
  the remote side).
- The AudioWorklet module lives in `web/public/pcm-worklet.js` so `vite build` copies it
  verbatim to `dist/`; it mixes, low-passes, resamples to 16 kHz and cuts 3200-sample frames.
- Client reconnect: 60 s ring buffer, replay from `started.next_seq`. Dead-socket
  detection must look at **no progress** of `bufferedAmount` for a while, not a size
  threshold alone — the post-resume replay legitimately fills the buffer.
- Async permission prompts can outlive the page: re-check an `unmounted` flag after each
  `await` and release tracks / AudioContext / ws.
