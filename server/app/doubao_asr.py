"""Doubao (Volcengine) streaming ASR v3 "sauc" client.

Protocol reference: .trellis/tasks/10-18-recbean-mvp/research/doubao-streaming-asr-raw.md

Binary frame = 4-byte header + [optional int32 sequence] + uint32 payload size + payload.
All integers are BIG-endian.

    byte0: protocol version (4b, =1) | header size in 4-byte words (4b, =1)
    byte1: message type (4b)          | message type specific flags (4b)
    byte2: serialization (4b)         | compression (4b)
    byte3: reserved

We send the full client request with sequence 1 and audio packets with sequence 2, 3, ...
(flags 0b0001); the last audio packet uses flags 0b0011 and a negative sequence.
Payloads are gzip-compressed; the request is JSON, audio is raw s16le PCM.
"""

from __future__ import annotations

import asyncio
import gzip
import json
import logging
import struct
import uuid
from dataclasses import dataclass, field
from typing import Any, AsyncIterator, Callable, Optional

from .asr import AsrError, AsrEvent, Emit, FinalEvent, PartialEvent

log = logging.getLogger("recbean.doubao")

PROTOCOL_VERSION = 0b0001
HEADER_WORDS = 0b0001

# message types
FULL_CLIENT_REQUEST = 0b0001
AUDIO_ONLY_REQUEST = 0b0010
FULL_SERVER_RESPONSE = 0b1001
SERVER_ERROR_RESPONSE = 0b1111

# message type specific flags
FLAG_NONE = 0b0000
FLAG_POS_SEQUENCE = 0b0001
FLAG_LAST_NO_SEQUENCE = 0b0010
FLAG_NEG_SEQUENCE = 0b0011
FLAG_EVENT = 0b0100

# serialization / compression
SER_NONE = 0b0000
SER_JSON = 0b0001
COMP_NONE = 0b0000
COMP_GZIP = 0b0001

SUCCESS_CODE = 20000000


def build_header(msg_type: int, flags: int, serialization: int, compression: int) -> bytes:
    return bytes(
        (
            (PROTOCOL_VERSION << 4) | HEADER_WORDS,
            ((msg_type & 0x0F) << 4) | (flags & 0x0F),
            ((serialization & 0x0F) << 4) | (compression & 0x0F),
            0,
        )
    )


def encode_full_client_request(params: dict, seq: Optional[int] = 1) -> bytes:
    payload = gzip.compress(json.dumps(params, ensure_ascii=False).encode("utf-8"))
    flags = FLAG_POS_SEQUENCE if seq is not None else FLAG_NONE
    out = bytearray(build_header(FULL_CLIENT_REQUEST, flags, SER_JSON, COMP_GZIP))
    if seq is not None:
        out += struct.pack(">i", seq)
    out += struct.pack(">I", len(payload)) + payload
    return bytes(out)


def encode_audio_request(pcm: bytes, seq: Optional[int], last: bool) -> bytes:
    """Audio-only request. With ``seq`` given, the last packet carries ``-seq``."""
    payload = gzip.compress(pcm)
    if seq is None:
        flags = FLAG_LAST_NO_SEQUENCE if last else FLAG_NONE
    else:
        flags = FLAG_NEG_SEQUENCE if last else FLAG_POS_SEQUENCE
    out = bytearray(build_header(AUDIO_ONLY_REQUEST, flags, SER_NONE, COMP_GZIP))
    if seq is not None:
        out += struct.pack(">i", -abs(seq) if last else seq)
    out += struct.pack(">I", len(payload)) + payload
    return bytes(out)


def encode_server_response(
    payload: Any, seq: Optional[int] = None, last: bool = False, compress: bool = True
) -> bytes:
    """Build a full server response frame (used by tests / fake servers)."""
    body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    if compress:
        body = gzip.compress(body)
    flags = 0
    if seq is not None:
        flags |= FLAG_POS_SEQUENCE
    if last:
        flags |= FLAG_LAST_NO_SEQUENCE
    out = bytearray(
        build_header(FULL_SERVER_RESPONSE, flags, SER_JSON, COMP_GZIP if compress else COMP_NONE)
    )
    if seq is not None:
        out += struct.pack(">i", seq)
    out += struct.pack(">I", len(body)) + body
    return bytes(out)


def encode_error_response(code: int, message: str) -> bytes:
    body = message.encode("utf-8")
    return (
        build_header(SERVER_ERROR_RESPONSE, FLAG_NONE, SER_JSON, COMP_NONE)
        + struct.pack(">II", code, len(body))
        + body
    )


@dataclass
class ServerMessage:
    msg_type: int
    flags: int
    serialization: int
    compression: int
    sequence: Optional[int] = None
    is_last: bool = False
    error_code: Optional[int] = None
    payload: Any = None  # parsed JSON (dict) / str / bytes
    raw_payload: bytes = b""

    @property
    def is_error(self) -> bool:
        return self.msg_type == SERVER_ERROR_RESPONSE


class ProtocolError(AsrError):
    pass


def decode_server_message(buf: bytes) -> ServerMessage:
    if len(buf) < 4:
        raise ProtocolError(f"frame too short ({len(buf)} B)")
    header_size = (buf[0] & 0x0F) * 4
    msg = ServerMessage(
        msg_type=buf[1] >> 4,
        flags=buf[1] & 0x0F,
        serialization=buf[2] >> 4,
        compression=buf[2] & 0x0F,
    )
    rest = memoryview(buf)[header_size:]
    try:
        if msg.flags & FLAG_POS_SEQUENCE:
            (msg.sequence,) = struct.unpack(">i", rest[:4])
            rest = rest[4:]
        if msg.flags & FLAG_LAST_NO_SEQUENCE:
            msg.is_last = True
        if msg.flags & FLAG_EVENT:
            rest = rest[4:]
        if msg.msg_type == FULL_SERVER_RESPONSE:
            (size,) = struct.unpack(">I", rest[:4])
            body = bytes(rest[4: 4 + size])
        elif msg.msg_type == SERVER_ERROR_RESPONSE:
            msg.error_code, size = struct.unpack(">II", rest[:8])
            body = bytes(rest[8: 8 + size])
        else:
            raise ProtocolError(f"unexpected message type {msg.msg_type:#x}")
    except struct.error as e:
        raise ProtocolError(f"truncated frame: {e}") from e
    if msg.compression == COMP_GZIP and body:
        try:
            body = gzip.decompress(body)
        except OSError as e:
            if msg.msg_type != SERVER_ERROR_RESPONSE:
                raise ProtocolError(f"bad gzip payload: {e}") from e
    msg.raw_payload = body
    if msg.serialization == SER_JSON and body:
        try:
            msg.payload = json.loads(body.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            msg.payload = body.decode("utf-8", "replace")
    else:
        msg.payload = body
    return msg


def build_request_params(uid: str = "recbean", extra_request: Optional[dict] = None) -> dict:
    request = {
        "model_name": "bigmodel",
        "enable_punc": True,
        "enable_itn": True,
        "enable_speaker_info": True,
        "ssd_version": "200",
        "enable_nonstream": True,
        "show_utterances": True,
    }
    if extra_request:
        request.update(extra_request)
    return {
        "user": {"uid": uid, "platform": "Linux", "app_version": "recbean"},
        "audio": {"format": "pcm", "codec": "raw", "rate": 16000, "bits": 16, "channel": 1},
        "request": request,
    }


def extract_speaker(utt: dict) -> Optional[str]:
    """Return the raw speaker label of a definite utterance, or None.

    Confirmed against real bigmodel_async responses (2026-10-18, seedasr 2.0,
    enable_speaker_info=true / ssd_version="200", logid 20261018174705D5BEBDECACE961CA6BF5):
    the label is ``utterances[].additions.speaker_id``, a string starting at "0", present
    only on definite utterances. The other keys are kept as a tolerant fallback.
    Keep ALL speaker parsing in this one function.
    """
    candidates = []
    additions = utt.get("additions")
    if isinstance(additions, str):
        try:
            additions = json.loads(additions)
        except json.JSONDecodeError:
            additions = None
    if isinstance(additions, dict):
        candidates += [additions.get(k) for k in ("speaker", "speaker_id", "spk", "spk_id")]
    candidates += [utt.get(k) for k in ("speaker", "speaker_id", "spk", "spk_id")]
    for value in candidates:
        if value is None or value == "":
            continue
        if isinstance(value, dict):  # e.g. {"id": "1"}
            value = value.get("id", value.get("speaker"))
            if value is None:
                continue
        return str(value)
    return None


def _utterances(resp: Any) -> list:
    if not isinstance(resp, dict):
        return []
    result = resp.get("result")
    if isinstance(result, dict):
        return list(result.get("utterances") or [])
    if isinstance(result, list):  # table says list; example says dict - accept both
        out = []
        for r in result:
            if isinstance(r, dict):
                out += r.get("utterances") or []
        return out
    return []


class ResultTracker:
    """Turns successive full server responses into Partial/Final events.

    Works for both result_type "full" (all utterances repeated every time) and
    "single" (only new ones): definite utterances are deduped by (start, end).
    """

    def __init__(self) -> None:
        self._done: set = set()
        self._partial = ""

    def feed(self, resp: Any) -> list[AsrEvent]:
        events: list[AsrEvent] = []
        pending: list[str] = []
        for u in _utterances(resp):
            if not isinstance(u, dict):
                continue
            text = str(u.get("text") or "")
            if u.get("definite"):
                start = int(u.get("start_time") or 0)
                end = int(u.get("end_time") or start)
                key = (start, end)
                if key in self._done:
                    continue
                self._done.add(key)
                if text.strip():
                    events.append(FinalEvent(text, start, end, extract_speaker(u)))
            else:
                pending.append(text)
        partial = "".join(pending)
        if partial != self._partial:
            self._partial = partial
            events.append(PartialEvent(partial))
        return events


@dataclass
class DoubaoConfig:
    url: str
    resource_id: str
    app_key: str = ""
    access_key: str = ""
    api_key: str = ""
    uid: str = "recbean"
    extra_request: dict = field(default_factory=dict)
    open_timeout: float = 10.0


def build_auth_headers(cfg: DoubaoConfig, connect_id: str) -> dict:
    headers = {
        "X-Api-Resource-Id": cfg.resource_id,
        "X-Api-Connect-Id": connect_id,
        "X-Api-Request-Id": connect_id,
    }
    if cfg.api_key:
        headers["X-Api-Key"] = cfg.api_key
    else:
        headers["X-Api-App-Key"] = cfg.app_key
        headers["X-Api-Access-Key"] = cfg.access_key
    return headers


class DoubaoAsrEngine:
    """One instance per recording. See ``asr.AsrEngine``."""

    def __init__(
        self,
        cfg: DoubaoConfig,
        on_raw: Optional[Callable[[ServerMessage], None]] = None,
        packet_bytes: int = 6400,
    ) -> None:
        self.cfg = cfg
        self.on_raw = on_raw
        self.packet_bytes = packet_bytes  # 200 ms of 16 kHz s16le
        self.logid: Optional[str] = None
        self.connect_id = str(uuid.uuid4())

    async def _connect(self):
        from websockets.asyncio.client import connect
        from websockets.exceptions import InvalidHandshake, InvalidStatus

        headers = build_auth_headers(self.cfg, self.connect_id)
        try:
            ws = await connect(
                self.cfg.url,
                additional_headers=headers,
                max_size=None,
                open_timeout=self.cfg.open_timeout,
                compression=None,
            )
        except InvalidStatus as e:
            resp = e.response
            self.logid = resp.headers.get("X-Tt-Logid")
            body = (resp.body or b"")[:500].decode("utf-8", "replace")
            log.error(
                "doubao handshake rejected: HTTP %s logid=%s connect_id=%s body=%s",
                resp.status_code, self.logid, self.connect_id, body,
            )
            raise AsrError(f"豆包握手失败 HTTP {resp.status_code}: {body}") from e
        except (OSError, asyncio.TimeoutError, InvalidHandshake) as e:
            log.error("doubao connect failed: %r", e)
            raise AsrError(f"连接豆包失败: {e!r}") from e
        self.logid = ws.response.headers.get("X-Tt-Logid") if ws.response else None
        log.info("doubao connected logid=%s connect_id=%s", self.logid, self.connect_id)
        return ws

    async def _send_audio(self, ws, audio: AsyncIterator[bytes], seq: int) -> None:
        held: Optional[bytes] = None
        buf = bytearray()
        async for chunk in audio:
            buf += chunk
            while len(buf) >= self.packet_bytes:
                pkt = bytes(buf[: self.packet_bytes])
                del buf[: self.packet_bytes]
                if held is not None:
                    await ws.send(encode_audio_request(held, seq, last=False))
                    seq += 1
                held = pkt
        if buf:
            if held is not None:
                await ws.send(encode_audio_request(held, seq, last=False))
                seq += 1
            held = bytes(buf)
        await ws.send(encode_audio_request(held or b"", seq, last=True))
        log.info("doubao last packet sent seq=-%d logid=%s", seq, self.logid)

    async def run(self, audio: AsyncIterator[bytes], emit: Emit) -> None:
        from websockets.exceptions import ConnectionClosed

        ws = await self._connect()
        tracker = ResultTracker()
        sender: Optional[asyncio.Task] = None
        try:
            await ws.send(encode_full_client_request(build_request_params(
                self.cfg.uid, self.cfg.extra_request), seq=1))
            sender = asyncio.create_task(self._send_audio(ws, audio, seq=2))
            got_last = False
            try:
                async for raw in ws:
                    if isinstance(raw, str):
                        log.warning("doubao unexpected text frame: %s", raw[:200])
                        continue
                    msg = decode_server_message(raw)
                    if self.on_raw:
                        self.on_raw(msg)
                    if msg.is_error:
                        raise AsrError(f"豆包错误 {msg.error_code}: {msg.payload}")
                    if isinstance(msg.payload, dict):
                        code = msg.payload.get("code")
                        if code not in (None, 0, SUCCESS_CODE):
                            raise AsrError(f"豆包错误 {code}: {msg.payload.get('message')}")
                        for ev in tracker.feed(msg.payload):
                            await emit(ev)
                    if msg.is_last:
                        got_last = True
                        break
            except ConnectionClosed as e:
                raise AsrError(f"豆包连接断开: code={e.code} reason={e.reason}") from e
            if sender.done() and sender.exception():
                raise AsrError(f"发送音频失败: {sender.exception()!r}")
            if not got_last:
                raise AsrError("豆包连接在最终结果前关闭")
            log.info("doubao session finished logid=%s", self.logid)
        except AsrError as e:
            log.error("doubao asr error logid=%s: %s", self.logid, e)
            raise
        finally:
            if sender and not sender.done():
                sender.cancel()
                try:
                    await sender
                except (asyncio.CancelledError, Exception):
                    pass
            await ws.close()


def engine_factory_from_settings(settings) -> Callable[[], Any]:
    from .asr import UnconfiguredAsrEngine

    def factory():
        if not settings.doubao_configured:
            return UnconfiguredAsrEngine()
        return DoubaoAsrEngine(
            DoubaoConfig(
                url=settings.doubao_url,
                resource_id=settings.doubao_resource_id,
                app_key=settings.doubao_app_key,
                access_key=settings.doubao_access_key,
                api_key=settings.doubao_api_key,
            )
        )

    return factory
