"""Generate shared IMA-ADPCM test vectors (server tests + firmware host tests).

    py -3 tools/gen_adpcm_vectors.py   # writes tests/fixtures/adpcm_vectors.json

Each vector: initial encoder state (predictor, step_index), input int16 PCM, expected
ADPCM bytes (hex, low nibble = earlier sample), the full device frame (hex, header +
data, see app/adpcm.py) for 3200-sample vectors, the decoded PCM and the end state.
"""

from __future__ import annotations

import json
import math
import sys
from array import array
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from app.adpcm import FRAME_SAMPLES, decode_block, encode_block, pack_frame  # noqa: E402


def lcg(seed: int):
    x = seed
    while True:
        x = (1103515245 * x + 12345) & 0x7FFFFFFF
        yield x


def speechlike(n: int, offset: int, seed: int) -> list[int]:
    rnd = lcg(seed)
    out = []
    for i in range(offset, offset + n):
        t = i / 16000
        env = 0.5 + 0.5 * math.sin(2 * math.pi * 3 * t)
        v = env * (8000 * math.sin(2 * math.pi * 440 * t) + 3000 * math.sin(2 * math.pi * 1500 * t))
        v += (next(rnd) % 1001) - 500
        out.append(max(-32768, min(32767, int(round(v)))))
    return out


def extremes(n: int) -> list[int]:
    out = []
    for i in range(n):
        phase = (i // 50) % 4
        out.append((32767, -32768, 0, 20000)[phase] if i % 7 else -32768 + (i % 3))
    return out


def vector(name: str, pcm: list[int], pred: int, idx: int, seq=None) -> dict:
    data, end_pred, end_idx = encode_block(pcm, pred, idx)
    decoded, dpred, didx = decode_block(data, pred, idx)
    assert (dpred, didx) == (end_pred, end_idx)
    v = {
        "name": name,
        "predictor": pred,
        "step_index": idx,
        "pcm": pcm,
        "adpcm_hex": data.hex(),
        "decoded": list(decoded),
        "end_predictor": end_pred,
        "end_step_index": end_idx,
    }
    if seq is not None:
        v["seq"] = seq
        v["frame_hex"] = pack_frame(seq, pred, idx, data).hex()
    return v, end_pred, end_idx


def main() -> None:
    vectors = []
    v, p, i = vector("speech_frame0", speechlike(FRAME_SAMPLES, 0, 1), 0, 0, seq=0)
    vectors.append(v)
    v, p, i = vector("speech_frame1_continues", speechlike(FRAME_SAMPLES, FRAME_SAMPLES, 2), p, i,
                     seq=1)
    vectors.append(v)
    v, _, _ = vector("extremes_clamp", extremes(FRAME_SAMPLES), 0, 0, seq=7)
    vectors.append(v)
    v, _, _ = vector("silence_from_state", [0] * FRAME_SAMPLES, -1234, 40, seq=0x01020304)
    vectors.append(v)
    v, _, _ = vector("short_block_16", [0, 100, 300, 800, 1600, 3000, 1000, -2000, -8000,
                                        -16000, -32768, 32767, 0, 5, -5, 12345], 0, 0)
    vectors.append(v)
    doc = {
        "description": "IMA-ADPCM vectors for recbean (design.md section 3). Standard IMA "
                       "step/index tables. Codes packed low nibble first. frame_hex = "
                       "seq u32 LE + predictor i16 LE + step_index u8 + reserved u8 + data. "
                       "predictor/step_index are the state BEFORE the first sample.",
        "sample_rate": 16000,
        "frame_samples": FRAME_SAMPLES,
        "vectors": vectors,
    }
    out = ROOT / "tests" / "fixtures" / "adpcm_vectors.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(doc, separators=(",", ":")) + "\n", encoding="utf-8")
    print(f"wrote {out} ({out.stat().st_size} bytes, {len(vectors)} vectors)")


if __name__ == "__main__":
    main()
