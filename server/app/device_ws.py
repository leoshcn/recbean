"""Device WebSocket gateway (design.md §3).

Endpoint ``/device/ws`` with ``Authorization: Bearer <device_token>``.

Device -> server (text JSON unless noted)::

    {"type":"start","sample_rate":16000,"ch":1,"codec":"ima_adpcm","fw":"x.y.z",
     "mode":"meeting|note"}
    <binary ADPCM frame, see adpcm.py>
    {"type":"stop"}
    {"type":"resume","recording_id":"...","next_seq":N}

Server -> device::

    {"type":"started","recording_id":"..."}           (+ "resumed":true,"next_seq":N on resume)
    {"type":"partial","text":"..."}
    {"type":"final","idx":n,"speaker":1,"start_ms":..,"end_ms":..,"text":"..."}
    {"type":"error","code":"asr_error|auth|busy","msg":"..."}
    {"type":"saved","recording_id":"...","duration_ms":..,"sentences":n}

``mode`` is optional: missing or unknown values mean ``meeting`` (old firmware). A
``note`` recording runs ASR without speaker diarization and all its sentences belong to
speaker 1. Both modes send the currently active hotwords with the ASR request.

Additional error codes (outside the v1 table, device may treat as generic error):
``bad_request`` (malformed/unsupported start), ``not_recording`` (stop without an
active recording), ``resume_failed`` (unknown recording id on resume).
"""

from __future__ import annotations

import asyncio
import json
import logging
import secrets
from datetime import datetime
from typing import Callable, Optional
from zoneinfo import ZoneInfo

from fastapi import WebSocket
from starlette.websockets import WebSocketDisconnect

from .adpcm import FRAME_SAMPLES, FrameError, decode_frame
from .asr import AsrFactory, FinalEvent, PartialEvent
from .config import Settings
from .hotwords import active_words
from .pairing import hash_token
from .store import Store
from .wav import WavWriter

log = logging.getLogger("recbean.device")

DEVICE_TEXT_MAX = 256  # bytes, design.md §3
MAX_GAP_FRAMES = 300  # fill at most 60 s of silence for a sequence gap
DURATION_UPDATE_MS = 5000
TITLE_PREFIX = {"meeting": "会议", "note": "笔记"}

# ``decode(buf) -> (seq, pcm)``: one binary ws frame -> sequence number + 16 kHz s16le PCM
# (FRAME_SAMPLES samples). Raises ``FrameError`` for malformed frames.
FrameDecoder = Callable[[bytes], tuple[int, bytes]]


def decode_adpcm_frame(buf: bytes) -> tuple[int, bytes]:
    """Device frames: IMA-ADPCM with seq + codec state header (adpcm.py)."""
    frame, pcm = decode_frame(buf)
    return frame.seq, pcm


def parse_mode(value) -> str:
    """Device start ``mode`` -> recording kind; missing/unknown means meeting."""
    return "note" if value == "note" else "meeting"


def truncate_head(text: str, limit: int = DEVICE_TEXT_MAX) -> str:
    b = text.encode("utf-8")
    return text if len(b) <= limit else b[:limit].decode("utf-8", "ignore")


def truncate_tail(text: str, limit: int = DEVICE_TEXT_MAX) -> str:
    """Keep the most recent part (for in-progress partial lines)."""
    b = text.encode("utf-8")
    return text if len(b) <= limit else b[-limit:].decode("utf-8", "ignore")


class SpeakerMapper:
    """Map raw ASR speaker labels to 1-based indexes in order of first appearance.

    With ``single=True`` (note mode) every sentence maps to speaker 1.
    """

    def __init__(self, single: bool = False) -> None:
        self._map: dict[str, int] = {}
        self._last = 1
        self.single = single

    def idx(self, label: Optional[str]) -> int:
        if self.single:
            return 1
        if label is None:
            return self._last
        if label not in self._map:
            self._map[label] = len(self._map) + 1
        self._last = self._map[label]
        return self._last


class DeviceConn:
    def __init__(self, ws: WebSocket, device_id: str) -> None:
        self.ws = ws
        self.device_id = device_id
        self._lock = asyncio.Lock()
        self.alive = True

    async def send(self, event: dict) -> bool:
        if not self.alive:
            return False
        data = json.dumps(event, ensure_ascii=False, separators=(",", ":"))
        try:
            async with self._lock:
                await self.ws.send_text(data)
            return True
        except Exception as e:  # connection gone
            log.info("send to device %s failed: %r", self.device_id, e)
            self.alive = False
            return False


class RecordingSession:
    def __init__(self, gw: "Gateway", rec_id: str, device_id: str, kind: str = "meeting",
                 hotwords: Optional[list[str]] = None,
                 decode: FrameDecoder = decode_adpcm_frame) -> None:
        self.gw = gw
        self.decode = decode
        self.store = gw.store
        self.rec_id = rec_id
        self.device_id = device_id
        self.kind = kind
        # Snapshot at start: hotword edits only affect recordings started afterwards.
        self.hotwords = list(hotwords or [])
        self.wav = WavWriter(gw.settings.audio_dir / f"{rec_id}.wav")
        self.next_seq = 0
        self.conn: Optional[DeviceConn] = None
        self.pending: list[dict] = []
        self.audio_q: asyncio.Queue = asyncio.Queue()
        self.asr_task: Optional[asyncio.Task] = None
        self.asr_failed = False
        self.asr_closed = False
        self.speakers = SpeakerMapper(single=(kind == "note"))
        self.finished = False
        self.resume_timer: Optional[asyncio.Task] = None
        self.last_partial = ""
        self._last_duration_update = 0

    # ---- device events --------------------------------------------------
    async def push(self, event: dict, buffer: bool = True) -> None:
        if self.conn is not None and await self.conn.send(event):
            return
        if buffer:
            self.pending.append(event)

    async def attach(self, conn: DeviceConn) -> None:
        if self.resume_timer:
            self.resume_timer.cancel()
            self.resume_timer = None
        self.conn = conn
        pending, self.pending = self.pending, []
        for ev in pending:
            await self.push(ev)

    def detach(self) -> None:
        self.conn = None
        if self.finished or self.resume_timer:
            return
        log.info("recording %s: device disconnected, waiting %.0fs for resume",
                 self.rec_id, self.gw.settings.resume_timeout_s)
        self.resume_timer = asyncio.create_task(self._resume_timeout())

    async def _resume_timeout(self) -> None:
        await asyncio.sleep(self.gw.settings.resume_timeout_s)
        self.resume_timer = None
        log.warning("recording %s: no resume, marking interrupted", self.rec_id)
        await self.finish(interrupted=True)

    # ---- audio ------------------------------------------------------------
    def _feed(self, pcm: bytes) -> None:
        self.wav.write(pcm)
        if not self.asr_failed and not self.asr_closed:
            self.audio_q.put_nowait(pcm)

    def on_frame(self, buf: bytes) -> None:
        if self.finished:
            return
        try:
            seq, pcm = self.decode(buf)
        except FrameError as e:
            log.warning("recording %s: bad frame (%d B): %s", self.rec_id, len(buf), e)
            return
        if seq < self.next_seq:
            return  # duplicate (resent after reconnect)
        gap = seq - self.next_seq
        if gap:
            log.warning("recording %s: seq gap %d..%d, filling %d frames of silence",
                        self.rec_id, self.next_seq, seq - 1, min(gap, MAX_GAP_FRAMES))
            silence = bytes(FRAME_SAMPLES * 2)
            for _ in range(min(gap, MAX_GAP_FRAMES)):
                self._feed(silence)
        self._feed(pcm)
        self.next_seq = seq + 1
        dur = self.wav.duration_ms
        if dur - self._last_duration_update >= DURATION_UPDATE_MS:
            self._last_duration_update = dur
            self.wav.flush_header()
            self.store.update_recording(self.rec_id, duration_ms=dur)

    # ---- ASR --------------------------------------------------------------
    async def _audio_iter(self):
        while True:
            chunk = await self.audio_q.get()
            if chunk is None:
                return
            yield chunk

    def start_asr(self) -> None:
        self.asr_task = asyncio.create_task(self._run_asr())

    async def _run_asr(self) -> None:
        try:
            engine = self.gw.asr_factory(self.kind, self.hotwords)
            await engine.run(self._audio_iter(), self._on_asr_event)
        except asyncio.CancelledError:
            raise
        except Exception as e:
            await self._asr_fail(str(e) or repr(e))

    async def _asr_fail(self, msg: str) -> None:
        log.error("recording %s: ASR failed: %s", self.rec_id, msg)
        self.asr_failed = True
        while not self.audio_q.empty():
            self.audio_q.get_nowait()
        self.store.update_recording(self.rec_id, error=msg[:500])
        await self.push({"type": "error", "code": "asr_error", "msg": truncate_head(msg, 128)})

    async def _on_asr_event(self, ev) -> None:
        if isinstance(ev, PartialEvent):
            text = truncate_tail(ev.text)
            if text != self.last_partial:
                self.last_partial = text
                if self.conn is not None:
                    await self.conn.send({"type": "partial", "text": text})
        elif isinstance(ev, FinalEvent):
            spk = self.speakers.idx(ev.speaker)
            row = self.store.add_sentence(self.rec_id, spk, ev.start_ms, ev.end_ms, ev.text)
            await self.push({
                "type": "final", "idx": row["idx"], "speaker": spk,
                "start_ms": ev.start_ms, "end_ms": ev.end_ms, "text": truncate_head(ev.text),
            })

    # ---- end --------------------------------------------------------------
    async def finish(self, interrupted: bool) -> tuple[int, int]:
        if self.finished:
            rec = self.store.get_recording(self.rec_id) or {}
            return rec.get("duration_ms", 0), self.store.count_sentences(self.rec_id)
        self.finished = True
        if self.resume_timer and self.resume_timer is not asyncio.current_task():
            self.resume_timer.cancel()
        self.resume_timer = None
        self.asr_closed = True
        self.audio_q.put_nowait(None)
        if self.asr_task:
            try:
                await asyncio.wait_for(self.asr_task, self.gw.settings.asr_finish_timeout_s)
            except asyncio.TimeoutError:
                log.warning("recording %s: ASR final result timeout", self.rec_id)
            except Exception as e:  # pragma: no cover - _run_asr swallows errors
                log.error("recording %s: ASR task error %r", self.rec_id, e)
        self.wav.close()
        duration = self.wav.duration_ms
        if self.asr_failed:
            status = "asr_error"
        elif interrupted:
            status = "interrupted"
        else:
            status = "done"
        self.store.update_recording(self.rec_id, status=status, duration_ms=duration)
        self.gw.sessions.pop(self.rec_id, None)
        n = self.store.count_sentences(self.rec_id)
        log.info("recording %s finished: status=%s duration=%dms sentences=%d",
                 self.rec_id, status, duration, n)
        return duration, n


class Gateway:
    """Device recording gateway. Subclasses (``web_ws.BrowserGateway``) override the
    hooks below to change auth, accepted audio format and resume ownership; session /
    ASR / WAV / resume-timeout handling is shared.
    """

    peer = "device"  # log label
    decode: FrameDecoder = staticmethod(decode_adpcm_frame)

    def __init__(self, settings: Settings, store: Store, asr_factory: AsrFactory,
                 sessions: Optional[dict[str, RecordingSession]] = None) -> None:
        self.settings = settings
        self.store = store
        self.asr_factory = asr_factory
        # May be shared between gateways so the REST API sees every live recording.
        self.sessions: dict[str, RecordingSession] = {} if sessions is None else sessions
        self.tz = _zone(settings.timezone)

    # ---- hooks ------------------------------------------------------------
    async def authenticate(self, ws: WebSocket) -> Optional[str]:
        """Accept the ws; return the connection's device id, or None after rejecting."""
        token = _bearer(ws)
        device = self.store.device_by_token_hash(hash_token(token)) if token else None
        await ws.accept()
        if not device:
            log.warning("device ws rejected: bad token from %s", ws.client)
            try:
                await ws.send_text(json.dumps(
                    {"type": "error", "code": "auth", "msg": "device not paired"}))
                await ws.close(code=1008)
            except Exception:
                pass
            return None
        return device["id"]

    def check_format(self, msg: dict) -> Optional[str]:
        """Error message if the start's audio format is unsupported, else None."""
        if (msg.get("codec", "ima_adpcm"), msg.get("sample_rate", 16000),
                msg.get("ch", 1)) != ("ima_adpcm", 16000, 1):
            return "only ima_adpcm 16000 Hz mono is supported"
        return None

    def start_kind(self, msg: dict) -> str:
        return parse_mode(msg.get("mode"))

    async def before_start(self, conn: DeviceConn) -> None:
        # A new start from the same device abandons any earlier unfinished recording.
        for s in list(self.sessions.values()):
            if s.device_id == conn.device_id:
                await s.finish(interrupted=True)

    def started_extra(self, session: RecordingSession) -> dict:
        """Extra fields for the ``started`` event of a new recording."""
        return {}

    def owns(self, rec_id: str, owner_device_id: Optional[str], conn: DeviceConn,
             msg: dict) -> bool:
        """May ``conn`` resume recording ``rec_id`` (stored with ``owner_device_id``)?"""
        return owner_device_id == conn.device_id

    def _new_id(self, kind: str = "meeting") -> tuple[str, str]:
        now = datetime.now(self.tz)
        prefix = TITLE_PREFIX.get(kind, TITLE_PREFIX["meeting"])
        return (now.strftime("%Y%m%d-%H%M%S-") + secrets.token_hex(2),
                now.strftime(f"{prefix} %m-%d %H:%M"))

    def active_hotwords(self) -> list[str]:
        words = [h["word"] for h in self.store.list_hotwords()]
        return active_words(words, self.settings.hotword_token_budget)

    async def start(self, conn: DeviceConn, msg: dict) -> Optional[RecordingSession]:
        err = self.check_format(msg)
        if err:
            await conn.send({"type": "error", "code": "bad_request", "msg": err})
            return None
        await self.before_start(conn)
        kind = self.start_kind(msg)
        rec_id, title = self._new_id(kind)
        audio_path = self.settings.audio_dir / f"{rec_id}.wav"
        self.store.create_recording(rec_id, title, conn.device_id, str(audio_path), kind=kind)
        hotwords = self.active_hotwords()
        session = RecordingSession(self, rec_id, conn.device_id, kind=kind, hotwords=hotwords,
                                   decode=self.decode)
        self.sessions[rec_id] = session
        session.conn = conn
        log.info("recording %s (%s) started by %s %s fw=%s hotwords=%d", rec_id, kind,
                 self.peer, conn.device_id, msg.get("fw") or msg.get("client"), len(hotwords))
        await conn.send({"type": "started", "recording_id": rec_id,
                         **self.started_extra(session)})
        session.start_asr()
        return session

    async def resume(self, conn: DeviceConn, msg: dict) -> Optional[RecordingSession]:
        rec_id = str(msg.get("recording_id") or "")
        session = self.sessions.get(rec_id)
        if (session and not session.finished
                and self.owns(rec_id, session.device_id, conn, msg)):
            log.info("recording %s resumed (device next_seq=%s, server next_seq=%d)",
                     rec_id, msg.get("next_seq"), session.next_seq)
            await conn.send({"type": "started", "recording_id": rec_id, "resumed": True,
                             "next_seq": session.next_seq})
            await session.attach(conn)
            return session
        rec = self.store.get_recording(rec_id)
        if rec and self.owns(rec_id, rec["device_id"], conn, msg):
            # Already finalized (e.g. resume timeout): tell the device it is saved.
            await conn.send({"type": "saved", "recording_id": rec_id,
                             "duration_ms": rec["duration_ms"],
                             "sentences": self.store.count_sentences(rec_id)})
        else:
            await conn.send({"type": "error", "code": "resume_failed", "msg": "unknown recording"})
        return None

    async def shutdown(self) -> None:
        for s in list(self.sessions.values()):
            await s.finish(interrupted=True)

    async def handle(self, ws: WebSocket) -> None:
        device_id = await self.authenticate(ws)
        if device_id is None:
            return
        conn = DeviceConn(ws, device_id)
        log.info("%s %s connected from %s", self.peer, device_id, ws.client)
        session: Optional[RecordingSession] = None
        try:
            while True:
                msg = await ws.receive()
                if msg["type"] == "websocket.disconnect":
                    break
                data = msg.get("bytes")
                if data is not None:
                    if session is not None and not session.finished:
                        session.on_frame(data)
                    continue
                text = msg.get("text")
                if text is None:
                    continue
                try:
                    obj = json.loads(text)
                    kind = obj.get("type")
                except (json.JSONDecodeError, AttributeError):
                    await conn.send({"type": "error", "code": "bad_request", "msg": "bad json"})
                    continue
                if kind == "start":
                    if session is not None and not session.finished:
                        await conn.send({"type": "error", "code": "busy",
                                         "msg": "recording already in progress"})
                        continue
                    session = await self.start(conn, obj)
                elif kind == "stop":
                    if session is None or session.finished:
                        await conn.send({"type": "error", "code": "not_recording",
                                         "msg": "no active recording"})
                        continue
                    duration, n = await session.finish(interrupted=False)
                    await conn.send({"type": "saved", "recording_id": session.rec_id,
                                     "duration_ms": duration, "sentences": n})
                    session = None
                elif kind == "resume":
                    resumed = await self.resume(conn, obj)
                    # A failed resume (resume_failed / saved) must not drop this
                    # connection's active recording, or it would never be detached.
                    if resumed is not None:
                        session = resumed
                elif kind == "ping":
                    await conn.send({"type": "pong"})
                else:
                    log.info("%s %s: unknown message type %r", self.peer, device_id, kind)
        except WebSocketDisconnect:
            pass
        finally:
            conn.alive = False
            log.info("%s %s disconnected", self.peer, device_id)
            if session is not None and not session.finished and session.conn is conn:
                session.detach()


def _bearer(ws: WebSocket) -> str:
    auth = ws.headers.get("authorization", "")
    if auth.lower().startswith("bearer "):
        return auth[7:].strip()
    return ws.query_params.get("token", "")


def _zone(name: str):
    try:
        return ZoneInfo(name)
    except Exception:
        return None
