import json
import math
from array import array

import pytest

from app import adpcm
from app.adpcm import (FRAME_BYTES, FRAME_SAMPLES, AdpcmEncoder, FrameError, decode_block,
                       decode_frame, encode_block, iter_frames, pcm_bytes)
from conftest import FIXTURES

VECTORS = json.loads((FIXTURES / "adpcm_vectors.json").read_text(encoding="utf-8"))["vectors"]


def test_tables_are_standard_ima():
    assert len(adpcm.STEP_TABLE) == 89
    assert adpcm.STEP_TABLE[0] == 7 and adpcm.STEP_TABLE[-1] == 32767
    assert adpcm.INDEX_TABLE == (-1, -1, -1, -1, 2, 4, 6, 8) * 2
    assert FRAME_BYTES == 1608


@pytest.mark.parametrize("v", VECTORS, ids=[v["name"] for v in VECTORS])
def test_vectors_encode_and_decode(v):
    data, pred, idx = encode_block(v["pcm"], v["predictor"], v["step_index"])
    assert data.hex() == v["adpcm_hex"]
    assert (pred, idx) == (v["end_predictor"], v["end_step_index"])
    decoded, dpred, didx = decode_block(bytes.fromhex(v["adpcm_hex"]), v["predictor"],
                                        v["step_index"])
    assert list(decoded) == v["decoded"]
    assert (dpred, didx) == (pred, idx)
    if "frame_hex" in v:
        frame, pcm = decode_frame(bytes.fromhex(v["frame_hex"]))
        assert frame.seq == v["seq"] and frame.samples == FRAME_SAMPLES
        assert pcm == pcm_bytes(array("h", v["decoded"]))


def test_frame_header_layout():
    v = next(v for v in VECTORS if v["name"] == "silence_from_state")
    raw = bytes.fromhex(v["frame_hex"])
    assert len(raw) == FRAME_BYTES
    assert raw[:4] == bytes([0x04, 0x03, 0x02, 0x01])  # seq 0x01020304 LE
    assert int.from_bytes(raw[4:6], "little", signed=True) == -1234
    assert raw[6] == 40 and raw[7] == 0


def test_continuous_encoder_frames_decode_independently():
    n = FRAME_SAMPLES * 3
    src = array("h", (int(10000 * math.sin(2 * math.pi * 300 * i / 16000)) for i in range(n)))
    frames = list(iter_frames(pcm_bytes(src)))
    assert len(frames) == 3
    # decode frames out of order / individually
    out = {}
    for f in reversed(frames):
        frame, pcm = decode_frame(f)
        out[frame.seq] = pcm
    joined = b"".join(out[i] for i in range(3))
    dec = array("h")
    dec.frombytes(joined)
    noise = sum((a - b) ** 2 for a, b in zip(src[400:], dec[400:]))
    signal = sum(a * a for a in src[400:])
    assert 10 * math.log10(signal / noise) > 20  # ADPCM SNR on a clean tone


def test_frame_errors():
    with pytest.raises(FrameError):
        decode_frame(b"\x00" * 5)
    with pytest.raises(FrameError):
        decode_frame(b"\x00" * 6 + bytes([89, 0]) + b"\x00" * 10)
    with pytest.raises(FrameError):
        decode_frame(b"\x00" * (FRAME_BYTES + 2))


def test_encoder_seq_increments():
    enc = AdpcmEncoder()
    a = enc.encode_frame([0] * FRAME_SAMPLES)
    b = enc.encode_frame([0] * FRAME_SAMPLES)
    assert decode_frame(a)[0].seq == 0 and decode_frame(b)[0].seq == 1
