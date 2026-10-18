"""IMA-ADPCM codec and device audio frame format (design.md §3).

Device frame layout (all little-endian), 1608 bytes for a full 200 ms frame::

    offset 0  uint32  seq          frame sequence number (per recording, starts at 0)
    offset 4  int16   predictor    decoder predictor BEFORE the first sample of this frame
    offset 6  uint8   step_index   decoder step index (0..88) BEFORE the first sample
    offset 7  uint8   reserved     0
    offset 8  1600 B  data         3200 x 4-bit codes; sample 2k is the LOW nibble of
                                   byte k, sample 2k+1 the HIGH nibble (WAV/IMA order)

Every frame carries the codec state it starts from, so each frame decodes on its own
(needed for reconnect/resend and dedupe). The header predictor is *state*, not an
emitted sample: a frame always yields exactly 2 * len(data) samples.

The step/index tables and the encode/decode arithmetic are the standard IMA ADPCM
ones; the encoder's reconstruction is bit-identical to the decoder, so a continuously
running encoder can stamp its current state into each frame header.
Shared test vectors: server/tests/fixtures/adpcm_vectors.json.
"""

from __future__ import annotations

import struct
from array import array
from dataclasses import dataclass

STEP_TABLE = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
)
INDEX_TABLE = (-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8)

SAMPLE_RATE = 16000
FRAME_SAMPLES = 3200  # 200 ms
FRAME_DATA_BYTES = FRAME_SAMPLES // 2  # 1600
FRAME_HEADER = struct.Struct("<IhBB")  # seq, predictor, step_index, reserved
FRAME_HEADER_BYTES = FRAME_HEADER.size  # 8
FRAME_BYTES = FRAME_HEADER_BYTES + FRAME_DATA_BYTES  # 1608


class FrameError(ValueError):
    pass


@dataclass
class AdpcmState:
    predictor: int = 0
    step_index: int = 0


def decode_block(data: bytes, predictor: int, step_index: int) -> tuple[array, int, int]:
    """Decode nibbles (low nibble first). Returns (int16 samples, predictor, step_index)."""
    if not 0 <= step_index <= 88:
        raise FrameError(f"step_index out of range: {step_index}")
    out = array("h", bytes(4 * len(data)))
    pred = predictor
    idx = step_index
    steps = STEP_TABLE
    itab = INDEX_TABLE
    i = 0
    for byte in data:
        for code in (byte & 0x0F, byte >> 4):
            step = steps[idx]
            diff = step >> 3
            if code & 4:
                diff += step
            if code & 2:
                diff += step >> 1
            if code & 1:
                diff += step >> 2
            if code & 8:
                pred -= diff
                if pred < -32768:
                    pred = -32768
            else:
                pred += diff
                if pred > 32767:
                    pred = 32767
            idx += itab[code]
            if idx < 0:
                idx = 0
            elif idx > 88:
                idx = 88
            out[i] = pred
            i += 1
    return out, pred, idx


def encode_block(samples, predictor: int, step_index: int) -> tuple[bytes, int, int]:
    """Encode int16 samples (even count). Returns (data, predictor, step_index)."""
    n = len(samples)
    if n % 2:
        raise ValueError("sample count must be even")
    out = bytearray(n // 2)
    pred = predictor
    idx = step_index
    steps = STEP_TABLE
    itab = INDEX_TABLE
    for i in range(n):
        step = steps[idx]
        diff = samples[i] - pred
        code = 0
        if diff < 0:
            code = 8
            diff = -diff
        delta = step >> 3
        if diff >= step:
            code |= 4
            diff -= step
            delta += step
        step >>= 1
        if diff >= step:
            code |= 2
            diff -= step
            delta += step
        step >>= 1
        if diff >= step:
            code |= 1
            delta += step
        if code & 8:
            pred -= delta
            if pred < -32768:
                pred = -32768
        else:
            pred += delta
            if pred > 32767:
                pred = 32767
        idx += itab[code]
        if idx < 0:
            idx = 0
        elif idx > 88:
            idx = 88
        if i & 1:
            out[i >> 1] |= code << 4
        else:
            out[i >> 1] = code
    return bytes(out), pred, idx


@dataclass
class Frame:
    seq: int
    predictor: int
    step_index: int
    data: bytes

    @property
    def samples(self) -> int:
        return 2 * len(self.data)


def parse_frame(buf: bytes) -> Frame:
    if len(buf) < FRAME_HEADER_BYTES:
        raise FrameError(f"frame too short: {len(buf)} bytes")
    seq, pred, idx, _ = FRAME_HEADER.unpack_from(buf, 0)
    data = bytes(buf[FRAME_HEADER_BYTES:])
    if len(data) > FRAME_DATA_BYTES:
        raise FrameError(f"frame data too long: {len(data)} bytes")
    if idx > 88:
        raise FrameError(f"step_index out of range: {idx}")
    return Frame(seq, pred, idx, data)


def decode_frame(buf: bytes) -> tuple[Frame, bytes]:
    """Parse a device frame and return (frame, PCM s16le bytes)."""
    frame = parse_frame(buf)
    pcm, _, _ = decode_block(frame.data, frame.predictor, frame.step_index)
    return frame, pcm_bytes(pcm)


def pack_frame(seq: int, predictor: int, step_index: int, data: bytes) -> bytes:
    return FRAME_HEADER.pack(seq & 0xFFFFFFFF, predictor, step_index, 0) + data


class AdpcmEncoder:
    """Continuous encoder producing independently decodable device frames."""

    def __init__(self) -> None:
        self.state = AdpcmState()
        self.seq = 0

    def encode_frame(self, samples) -> bytes:
        pred, idx = self.state.predictor, self.state.step_index
        data, self.state.predictor, self.state.step_index = encode_block(samples, pred, idx)
        frame = pack_frame(self.seq, pred, idx, data)
        self.seq += 1
        return frame


def pcm_bytes(samples: array) -> bytes:
    """int16 array -> little-endian bytes."""
    import sys

    if sys.byteorder != "little":
        samples = array("h", samples)
        samples.byteswap()
    return samples.tobytes()


def pcm_samples(raw: bytes) -> array:
    """little-endian s16 bytes -> int16 array."""
    import sys

    a = array("h")
    a.frombytes(raw[: len(raw) - (len(raw) % 2)])
    if sys.byteorder != "little":
        a.byteswap()
    return a


def iter_frames(pcm: bytes, encoder: AdpcmEncoder | None = None):
    """Split s16le PCM into device frames (last frame zero-padded to 3200 samples)."""
    enc = encoder or AdpcmEncoder()
    samples = pcm_samples(pcm)
    for off in range(0, len(samples), FRAME_SAMPLES):
        chunk = samples[off: off + FRAME_SAMPLES]
        if len(chunk) < FRAME_SAMPLES:
            chunk = chunk + array("h", bytes(2 * (FRAME_SAMPLES - len(chunk))))
        yield enc.encode_frame(chunk)
