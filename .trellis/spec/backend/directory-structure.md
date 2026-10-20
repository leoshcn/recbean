# Backend Directory Structure

The backend is a single FastAPI app in `server/` (Python 3, no package build). One process
serves the device WebSocket, the browser WebSocket, the REST API and the built Web UI on one
port (`server/app/main.py` docstring).

## Layout

```
server/
├── app/                    # application package (imported as `app.*`)
│   ├── main.py             # create_app(settings, asr_factory) + default_app() (uvicorn --factory)
│   ├── config.py           # Settings dataclass from env, setup_logging()
│   ├── api.py              # REST routers: `api` (/api/*) and `device_http` (/device/*)
│   ├── models.py           # pydantic request bodies only
│   ├── store.py            # SQLite schema, migrations, all SQL (class Store)
│   ├── device_ws.py        # Gateway + RecordingSession: WS protocol, WAV, ASR, resume
│   ├── web_ws.py           # BrowserGateway(Gateway): browser PCM ingress
│   ├── asr.py              # ASR engine Protocol, events, AsrError, AsrFactory
│   ├── doubao_asr.py       # production ASR engine (Doubao binary WS protocol)
│   ├── hotwords.py         # pure functions: normalize/validate/estimate/select hotwords
│   ├── pairing.py          # PairingManager (in-memory codes + device tokens)
│   ├── adpcm.py, wav.py    # pure audio codecs / WAV header repair
├── tests/                  # pytest; conftest.py has FakeAsr, make_settings, helpers
│   └── fixtures/           # golden vectors (e.g. ADPCM) shared with firmware
├── tools/                  # dev scripts: fake_device.py, asr_probe.py, gen_adpcm_vectors.py
├── web/                    # Vue front end (see ../frontend/)
├── Dockerfile              # multi-stage: build web → python runtime with web/dist
```

## Where new code goes

| Adding… | Put it in | Example |
|---|---|---|
| REST endpoint | `api.py` on the `api` router; body model in `models.py` | `merge_speakers` + `SpeakerMerge` |
| SQL / schema change | `store.py` only (method on `Store`) | `Store.merge_speakers` |
| New env setting | field on `Settings` in `config.py` + `/.env.example` | `hotword_token_budget` |
| New audio ingress | subclass `Gateway` (see [browser-recording.md](./browser-recording.md)) | `web_ws.BrowserGateway` |
| New ASR engine | implement `asr.AsrEngine.run(audio, emit)`; wire via `AsrFactory` | `doubao_asr.DoubaoAsrEngine` |
| Pure logic (no I/O) | its own module with plain functions, unit-tested directly | `hotwords.py`, `adpcm.py` |

## Rules

- Shared objects are created once in `create_app()` and hung on `app.state`
  (`settings`, `store`, `gateway`, `browser_gateway`, `pairing`); handlers reach them via
  `request.app.state` (`api.py:_store`). No module-level singletons.
- `create_app()` takes `settings` and `asr_factory` parameters so tests inject `FakeAsr`
  and a temp `data_dir` (`tests/conftest.py:make_settings`). Keep new dependencies injectable
  the same way.
- Modules use relative imports (`from .store import KINDS`); tests and tools import `app.*`
  after adding `server/` to `sys.path`.
- Every module starts with a docstring stating its responsibility, and
  `from __future__ import annotations`.
