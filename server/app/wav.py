"""Streaming 16 kHz / 16-bit / mono WAV writer with header fix-up."""

from __future__ import annotations

import os
import struct
from pathlib import Path

HEADER_BYTES = 44


def wav_header(data_bytes: int, rate: int = 16000, channels: int = 1, bits: int = 16) -> bytes:
    block = channels * bits // 8
    return struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF", 36 + data_bytes, b"WAVE",
        b"fmt ", 16, 1, channels, rate, rate * block, block, bits,
        b"data", data_bytes,
    )


class WavWriter:
    def __init__(self, path: Path) -> None:
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        if self.path.exists() and self.path.stat().st_size >= HEADER_BYTES:
            self._f = open(self.path, "r+b")
            self._f.seek(0, os.SEEK_END)
            self.data_bytes = self._f.tell() - HEADER_BYTES
        else:
            self._f = open(self.path, "w+b")
            self._f.write(wav_header(0))
            self.data_bytes = 0

    @property
    def duration_ms(self) -> int:
        return self.data_bytes // 32  # 16000 Hz * 2 B = 32 B/ms

    def write(self, pcm: bytes) -> None:
        self._f.write(pcm)
        self.data_bytes += len(pcm)

    def flush_header(self) -> None:
        pos = self._f.tell()
        self._f.seek(0)
        self._f.write(wav_header(self.data_bytes))
        self._f.seek(pos)
        self._f.flush()

    def close(self) -> None:
        if self._f.closed:
            return
        self.flush_header()
        self._f.close()


def repair_wav(path: Path) -> int:
    """Rewrite the header from the file size (after a crash). Returns duration ms."""
    path = Path(path)
    if not path.exists():
        return 0
    size = path.stat().st_size
    if size < HEADER_BYTES:
        with open(path, "wb") as f:
            f.write(wav_header(0))
        return 0
    data = size - HEADER_BYTES
    data -= data % 2
    with open(path, "r+b") as f:
        f.write(wav_header(data))
    return data // 32
