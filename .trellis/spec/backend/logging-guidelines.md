# Logging Guidelines

## Setup

- stdlib `logging`, configured once by `config.setup_logging(level)` from `default_app()`
  (`LOG_LEVEL` env, default `INFO`); format
  `%(asctime)s %(levelname)s %(name)s: %(message)s`. Tests don't configure logging.
- One module logger, named under `recbean`:
  `recbean` (main), `recbean.api`, `recbean.device`, `recbean.doubao`. New modules:
  `log = logging.getLogger("recbean.<area>")`.

## Style

- Lazy %-formatting: `log.info("recording %s finished: status=%s", rec_id, status)` — never
  f-strings in log calls.
- Prefix with the subject id: `recording <id>: …`, `device <id> …`, `doubao … logid=%s`
  (logid/connect_id are what Doubao support asks for).
- Messages in English; user-facing text (errors stored in DB) may be Chinese.
- Use `%r` for exceptions to keep the type: `log.error("doubao connect failed: %r", e)`.

## Levels

| Level | Use | Examples |
|---|---|---|
| `info` | lifecycle events | started/resumed/finished recording, device connected/disconnected, paired, hotwords added |
| `warning` | recoverable anomalies | seq gap filled with silence, bad frame dropped, no resume → interrupted, ASR final timeout, rejected token |
| `error` | a feature failed | ASR failed, file delete failed, unexpected task error |

## Never log

- Device tokens, token hashes or Doubao keys. Pairing logs only the device id and the
  short-lived 4-digit code (`api.py`).
- Audio payloads or full transcripts. Truncate unexpected payloads (`raw[:200]`).
