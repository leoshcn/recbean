"""Streaming ASR engine interface used by the device gateway.

An engine consumes an async stream of 16 kHz s16le mono PCM chunks (ending when the
stream is exhausted) and reports results through ``emit``. It returns normally once the
final result for the last audio has been delivered and raises ``AsrError`` on failure.
Tests plug in a fake engine; production uses ``doubao_asr.DoubaoAsrEngine``.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import AsyncIterator, Awaitable, Callable, Optional, Protocol, Union


@dataclass
class PartialEvent:
    """In-progress sentence text (no speaker yet). Empty text clears the line."""

    text: str


@dataclass
class FinalEvent:
    """A finalized (definite) sentence. ``speaker`` is the raw ASR label or None."""

    text: str
    start_ms: int
    end_ms: int
    speaker: Optional[str] = None


AsrEvent = Union[PartialEvent, FinalEvent]
Emit = Callable[[AsrEvent], Awaitable[None]]


class AsrError(Exception):
    pass


class AsrEngine(Protocol):
    async def run(self, audio: AsyncIterator[bytes], emit: Emit) -> None: ...


AsrFactory = Callable[[], AsrEngine]


class UnconfiguredAsrEngine:
    """Used when no Doubao credentials are set: fails immediately (-> asr_error)."""

    async def run(self, audio: AsyncIterator[bytes], emit: Emit) -> None:
        raise AsrError("未配置豆包 ASR 凭据（DOUBAO_APP_KEY/DOUBAO_ACCESS_KEY 或 DOUBAO_API_KEY）")
