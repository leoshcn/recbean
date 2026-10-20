# Backend Quality Guidelines

## Testing (required)

- Run: `cd server && .venv/Scripts/python -m pytest` (Linux: `.venv/bin/python`); setup in
  `server/README.md`. All tests must pass before commit.
- Tests use the real app via `create_app(make_settings(tmp_path), asr_factory=FakeAsr)` and
  `fastapi.testclient.TestClient` — HTTP and WebSocket end to end, temp SQLite per test.
  Fixtures/helpers live in `tests/conftest.py`: `make_client`, `client`, `pair()`,
  `tone_frames()`, `collect_until()`, `FakeAsr` (records `(kind, hotwords)` in `FakeAsr.calls`).
- No network in tests: never hit Doubao; use `FakeAsr` (`fail_after=` for error paths).
  Real-API checks belong in `tools/asr_probe.py`.
- Every bug fix adds a regression test (e.g. resume ownership in `tests/test_web_ws.py`).
- Pure modules (`adpcm`, `hotwords`) get direct unit tests; ADPCM uses shared golden
  vectors in `tests/fixtures/` that the firmware also checks — regenerate with
  `tools/gen_adpcm_vectors.py`, never hand-edit.

## Code style

- Python 3 with type hints on all public functions; `Optional[...]`/`list[...]` style,
  `from __future__ import annotations` in every module.
- Line length ~100 (existing code). No formatter/linter is configured; match surrounding style.
- Module and non-trivial function docstrings explain *why* and reference design decisions
  (`design.md §6`, `D7`) or spec files.
- Config only via `Settings`; no `os.environ` reads elsewhere.
- Keep sync REST handlers sync (FastAPI threadpool) and the WS path async; the shared `Store`
  is lock-protected for that reason.

## Forbidden

- Forking `RecordingSession` logic per audio source — subclass `Gateway` hooks instead
  ([browser-recording.md](./browser-recording.md)).
- SQL outside `store.py`.
- Blocking network I/O inside async code; long waits must have timeouts
  (`asr_finish_timeout_s`, `resume_timeout_s`).
- New dependencies without adding them to `requirements.txt` (runtime) or
  `requirements-dev.txt` (tests).

## Review checklist

- [ ] Tests added/updated and `pytest` green
- [ ] Error codes/messages follow [error-handling.md](./error-handling.md)
- [ ] Logs follow [logging-guidelines.md](./logging-guidelines.md), no secrets
- [ ] Schema change has a migration in `Store._migrate()` and a test
- [ ] New setting documented in `.env.example`
- [ ] Protocol/API change reflected in `server/README.md` and the Web client (`web/src/api.js`)
