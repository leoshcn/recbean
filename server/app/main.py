"""recbean server: device WebSocket + REST API + static Web UI on one port (8080)."""

from __future__ import annotations

import logging
from contextlib import asynccontextmanager
from typing import Optional

from fastapi import FastAPI, WebSocket
from fastapi.responses import HTMLResponse
from fastapi.staticfiles import StaticFiles

from .api import api, device_http
from .asr import AsrFactory
from .config import Settings, setup_logging
from .device_ws import Gateway
from .doubao_asr import engine_factory_from_settings
from .pairing import PairingManager
from .store import Store
from .wav import repair_wav

log = logging.getLogger("recbean")


def recover_unfinished(store: Store) -> None:
    """Recordings left in 'recording' by a crash/restart become 'interrupted'."""
    for rec in store.recordings_with_status("recording"):
        duration = repair_wav(rec["audio_path"]) if rec.get("audio_path") else 0
        store.update_recording(rec["id"], status="interrupted", duration_ms=duration)
        log.warning("recording %s was unfinished at startup -> interrupted", rec["id"])


def create_app(settings: Optional[Settings] = None,
               asr_factory: Optional[AsrFactory] = None) -> FastAPI:
    settings = settings or Settings()
    settings.audio_dir.mkdir(parents=True, exist_ok=True)
    store = Store(settings.db_path)
    recover_unfinished(store)
    gateway = Gateway(settings, store, asr_factory or engine_factory_from_settings(settings))

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        log.info("recbean started: data_dir=%s asr_configured=%s resource=%s",
                 settings.data_dir, settings.doubao_configured, settings.doubao_resource_id)
        yield
        await gateway.shutdown()
        store.close()

    app = FastAPI(title="recbean", lifespan=lifespan)
    app.state.settings = settings
    app.state.store = store
    app.state.gateway = gateway
    app.state.pairing = PairingManager(store, settings.pairing_ttl_s)
    app.include_router(api)
    app.include_router(device_http)

    @app.websocket("/device/ws")
    async def device_ws(ws: WebSocket):
        await gateway.handle(ws)

    if settings.web_dist.is_dir():
        app.mount("/", StaticFiles(directory=str(settings.web_dist), html=True), name="web")
    else:
        @app.get("/", response_class=HTMLResponse)
        def no_web():
            return "<p>recbean server running. Web UI not built (server/web → npm run build).</p>"

    return app


def default_app() -> FastAPI:
    """Entry point: ``uvicorn app.main:default_app --factory``."""
    settings = Settings()
    setup_logging(settings.log_level)
    return create_app(settings)
