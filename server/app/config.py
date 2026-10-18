"""Settings read from environment variables (see /.env.example)."""

from __future__ import annotations

import logging
import os
from dataclasses import dataclass, field
from pathlib import Path


def _env(name: str, default: str = "") -> str:
    return os.environ.get(name, default).strip()


@dataclass
class Settings:
    data_dir: Path = field(default_factory=lambda: Path(_env("DATA_DIR", "./data")))
    web_dist: Path = field(
        default_factory=lambda: Path(
            _env("WEB_DIST", str(Path(__file__).resolve().parent.parent / "web" / "dist"))
        )
    )
    # Doubao credentials: old console uses APP_KEY + ACCESS_KEY, new console only API_KEY.
    doubao_app_key: str = field(default_factory=lambda: _env("DOUBAO_APP_KEY"))
    doubao_access_key: str = field(default_factory=lambda: _env("DOUBAO_ACCESS_KEY"))
    doubao_api_key: str = field(default_factory=lambda: _env("DOUBAO_API_KEY"))
    doubao_resource_id: str = field(
        default_factory=lambda: _env("DOUBAO_RESOURCE_ID") or "volc.seedasr.sauc.duration"
    )
    doubao_url: str = field(
        default_factory=lambda: _env("DOUBAO_URL")
        or "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async"
    )
    # Seconds the server keeps a recording open for a `resume` after an abrupt disconnect.
    resume_timeout_s: float = field(default_factory=lambda: float(_env("RESUME_TIMEOUT_S", "30")))
    # Max seconds to wait for ASR final results after stop.
    asr_finish_timeout_s: float = field(
        default_factory=lambda: float(_env("ASR_FINISH_TIMEOUT_S", "15"))
    )
    pairing_ttl_s: float = field(default_factory=lambda: float(_env("PAIRING_TTL_S", "600")))
    timezone: str = field(default_factory=lambda: _env("TZ_NAME", "Asia/Shanghai"))
    log_level: str = field(default_factory=lambda: _env("LOG_LEVEL", "INFO"))
    # Estimated-token budget for hotwords sent with every ASR session (app/hotwords.py).
    hotword_token_budget: int = field(
        default_factory=lambda: int(_env("HOTWORD_TOKEN_BUDGET", "100") or "100")
    )

    @property
    def db_path(self) -> Path:
        return self.data_dir / "recbean.db"

    @property
    def audio_dir(self) -> Path:
        return self.data_dir / "audio"

    @property
    def doubao_configured(self) -> bool:
        return bool(self.doubao_api_key or (self.doubao_app_key and self.doubao_access_key))


def setup_logging(level: str) -> None:
    logging.basicConfig(
        level=getattr(logging, level.upper(), logging.INFO),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
