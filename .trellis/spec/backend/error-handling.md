# Error Handling

## REST (`server/app/api.py`)

- Raise `fastapi.HTTPException(status, "short english detail")`; FastAPI returns
  `{"detail": "..."}`. The Web client surfaces `detail` verbatim (`web/src/api.js`).
- Status codes in use:
  | Code | When | Example |
  |---|---|---|
  | 400 | semantically invalid request | merge a speaker into itself |
  | 404 | entity missing — use `_recording_or_404()` helper first in every `/recordings/{id}` handler | `"recording not found"`, `"sentence not found"` |
  | 409 | conflicts with a live recording | delete/merge while `rec_id in gateway.sessions` → `"recording in progress"` |
  | 422 | validation; prefer pydantic `Field` constraints in `models.py`, manual check only for path/query params | `kind must be one of …`, `bad speaker idx` |
- Domain validators return a message instead of raising (`hotwords.validate_word -> Optional[str]`);
  the handler turns it into `HTTPException`. User-facing validation messages may be Chinese.
- Best-effort side effects (deleting the WAV) catch `OSError`, log at `error`, and still
  return success.

## WebSocket protocol (`device_ws.py`, `web_ws.py`)

- Errors are sent as events, not exceptions: `{"type":"error","code":...,"msg":...}` with
  codes `auth`, `bad_request`, `busy`, `not_recording`, `resume_failed`, `asr_error`
  (documented in the `device_ws.py` module docstring and `server/README.md`).
- Bad binary frames raise `FrameError` inside `decode()`; the session logs a warning and drops
  the frame — one bad frame never ends a recording.
- `DeviceConn.send()` never raises: on failure it logs at `info`, marks `alive=False`, returns
  `False`.
- `asyncio.CancelledError` is always re-raised (`except asyncio.CancelledError: raise`) before
  any broad `except Exception`.

## ASR

- Engines raise `asr.AsrError` (message may be Chinese, it is shown to users); wrap lower-level
  errors with `raise AsrError(...) from e` (`doubao_asr.py`).
- The session catches it, stores the message in `recording.error`, sets status `asr_error`,
  pushes an `asr_error` event (message truncated with `truncate_head(msg, 128)`), and **keeps
  recording audio** — ASR failure never loses the WAV.
- Missing credentials → `UnconfiguredAsrEngine` fails immediately, same path.

## Startup recovery

`main.recover_unfinished()` turns rows left in `recording` status into `interrupted`, repairing
the WAV header (`wav.repair_wav`). Any new long-running state must have an equivalent recovery.

## Don't

- Don't swallow exceptions without logging; don't use bare `except:`.
- Don't return 200 with an `{"error": ...}` body from REST — use status codes.
