"""Browser recording WebSocket gateway (task 10-20-web-browser-recording, design.md §2).

Endpoint ``/web/ws``. Same message semantics as the device protocol (device_ws.py) with
these differences:

- No token auth (the Web UI has no login and is LAN-only). Ownership of a recording is
  proven on resume by the random ``resume_token`` handed out in ``started``.
- ``start``: ``{"type":"start","codec":"pcm_s16le","sample_rate":16000,"ch":1,
  "client":"web/<ver>"}``; any other codec / rate / channel count -> ``bad_request``.
  Browser recordings are always ``kind=meeting`` (title ``会议 MM-DD HH:MM``) and stored
  with ``device_id="browser"``.
- ``started``: ``{"type":"started","recording_id":"...","resume_token":"..."}``
  (+ ``"resumed":true,"next_seq":N`` on resume, without a new token).
- Binary frame: ``seq u32 LE | 6400 B s16le PCM`` (3200 samples = 200 ms). Frames of any
  other length are logged and dropped.
- ``resume``: ``{"type":"resume","recording_id":"...","resume_token":"...","next_seq":N}``;
  wrong token / unknown recording -> ``resume_failed``; already finished -> ``saved``.

Several browser recordings may run concurrently; one connection holds at most one
recording (a second ``start`` -> ``busy``).
"""

from __future__ import annotations

import secrets
import struct
from typing import Optional

from fastapi import WebSocket

from .adpcm import FRAME_SAMPLES, FrameError
from .asr import AsrFactory
from .config import Settings
from .device_ws import DeviceConn, Gateway, RecordingSession
from .store import Store

BROWSER_DEVICE_ID = "browser"
SEQ = struct.Struct("<I")
PCM_FRAME_BYTES = SEQ.size + FRAME_SAMPLES * 2  # 6404


def decode_pcm_frame(buf: bytes) -> tuple[int, bytes]:
    """Browser frames: ``seq u32 LE`` + raw 16 kHz s16le mono PCM."""
    if len(buf) != PCM_FRAME_BYTES:
        raise FrameError(f"expected {PCM_FRAME_BYTES} bytes")
    return SEQ.unpack_from(buf)[0], bytes(buf[SEQ.size:])


class BrowserGateway(Gateway):
    peer = "web"
    decode = staticmethod(decode_pcm_frame)

    def __init__(self, settings: Settings, store: Store, asr_factory: AsrFactory,
                 sessions: Optional[dict[str, RecordingSession]] = None) -> None:
        super().__init__(settings, store, asr_factory, sessions)
        # rec_id -> resume token for browser recordings started by this process. Kept
        # after finish so a late resume can still be answered with ``saved``.
        self.tokens: dict[str, str] = {}

    async def authenticate(self, ws: WebSocket) -> Optional[str]:
        await ws.accept()
        return BROWSER_DEVICE_ID

    def check_format(self, msg: dict) -> Optional[str]:
        if (msg.get("codec"), msg.get("sample_rate", 16000),
                msg.get("ch", 1)) != ("pcm_s16le", 16000, 1):
            return "only pcm_s16le 16000 Hz mono is supported"
        return None

    def start_kind(self, msg: dict) -> str:
        return "meeting"

    async def before_start(self, conn: DeviceConn) -> None:
        pass  # browser recordings may run concurrently

    def started_extra(self, session: RecordingSession) -> dict:
        token = secrets.token_urlsafe(24)
        self.tokens[session.rec_id] = token
        return {"resume_token": token}

    def owns(self, rec_id: str, owner_device_id: Optional[str], conn: DeviceConn,
             msg: dict) -> bool:
        expected = self.tokens.get(rec_id)
        given = msg.get("resume_token")
        if owner_device_id != BROWSER_DEVICE_ID or not expected or not isinstance(given, str):
            return False
        return secrets.compare_digest(expected.encode(), given.encode())
