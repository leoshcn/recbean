"""Send a local 16 kHz WAV to the real Doubao streaming ASR and print raw responses.

Credentials come from the environment (or --env FILE in KEY=VALUE format):
DOUBAO_APP_KEY + DOUBAO_ACCESS_KEY (old console) or DOUBAO_API_KEY (new console),
DOUBAO_RESOURCE_ID (default volc.seedasr.sauc.duration), DOUBAO_URL (optional).

    py -3 tools/asr_probe.py two_speakers.wav               # PCM, raw JSON responses
    py -3 tools/asr_probe.py two_speakers.wav --adpcm       # round-trip through IMA-ADPCM
    py -3 tools/asr_probe.py two_speakers.wav --compare --ref ref.txt --quiet   # AC10

Use the raw output to confirm where the speaker label lives in definite utterances and
update ``extract_speaker`` in app/doubao_asr.py plus tests/fixtures/doubao_responses.json.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import re
import sys
import time
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from app.adpcm import decode_frame, iter_frames  # noqa: E402
from app.asr import FinalEvent  # noqa: E402
from app.config import Settings  # noqa: E402
from app.doubao_asr import DoubaoAsrEngine, DoubaoConfig, ServerMessage  # noqa: E402


def load_env_file(path: str) -> None:
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#") and "=" in line:
            k, v = line.split("=", 1)
            os.environ.setdefault(k.strip(), v.strip().strip('"').strip("'"))


def read_pcm(path: str) -> bytes:
    with wave.open(path) as w:
        if (w.getframerate(), w.getnchannels(), w.getsampwidth()) != (16000, 1, 2):
            raise SystemExit("need 16 kHz mono 16-bit WAV "
                             "(ffmpeg -i in -ar 16000 -ac 1 -sample_fmt s16 out.wav)")
        return w.readframes(w.getnframes())


def adpcm_roundtrip(pcm: bytes) -> bytes:
    out = b"".join(decode_frame(f)[1] for f in iter_frames(pcm))
    return out[: len(pcm)]


def edit_distance(a: str, b: str) -> int:
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb)))
        prev = cur
    return prev[-1]


def normalize(text: str) -> str:
    return re.sub(r"[\s，。！？、,.!?;；:：\"'“”‘’()（）-]", "", text).lower()


async def run_once(label: str, pcm: bytes, args) -> list[FinalEvent]:
    s = Settings()
    if not s.doubao_configured:
        raise SystemExit("set DOUBAO_APP_KEY + DOUBAO_ACCESS_KEY or DOUBAO_API_KEY")
    extra = json.loads(args.extra) if args.extra else {}
    cfg = DoubaoConfig(url=s.doubao_url, resource_id=s.doubao_resource_id,
                       app_key=s.doubao_app_key, access_key=s.doubao_access_key,
                       api_key=s.doubao_api_key, extra_request=extra)
    t0 = time.monotonic()

    def on_raw(msg: ServerMessage) -> None:
        if args.quiet:
            return
        payload = msg.payload
        print(f"--- [{label}] +{time.monotonic() - t0:6.2f}s type={msg.msg_type:#x} "
              f"flags={msg.flags:#06b} seq={msg.sequence} last={msg.is_last}")
        if isinstance(payload, dict) and not args.words:
            payload = json.loads(json.dumps(payload))
            for u in (payload.get("result") or {}).get("utterances") or []:
                if isinstance(u, dict):
                    u.pop("words", None)
        print(json.dumps(payload, ensure_ascii=False, indent=1) if isinstance(payload, dict)
              else payload)

    engine = DoubaoAsrEngine(cfg, on_raw=on_raw)
    finals: list[FinalEvent] = []

    async def emit(ev):
        if isinstance(ev, FinalEvent):
            finals.append(ev)

    async def audio():
        step = 6400
        interval = 0.2 / args.speed if args.speed > 0 else 0
        start = time.monotonic()
        for n, off in enumerate(range(0, len(pcm), step)):
            yield pcm[off: off + step]
            delay = start + (n + 1) * interval - time.monotonic()
            if delay > 0:
                await asyncio.sleep(delay)

    await engine.run(audio(), emit)
    print(f"=== [{label}] X-Tt-Logid={engine.logid} finals={len(finals)}")
    for f in finals:
        print(f"  [{f.start_ms / 1000:7.2f}-{f.end_ms / 1000:7.2f}] speaker={f.speaker!r} {f.text}")
    return finals


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("wav")
    ap.add_argument("--env", help="KEY=VALUE file with credentials (not committed!)")
    ap.add_argument("--adpcm", action="store_true", help="round-trip audio through IMA-ADPCM")
    ap.add_argument("--compare", action="store_true", help="run PCM and ADPCM, compare (AC10)")
    ap.add_argument("--ref", help="reference transcript (UTF-8 text) for CER")
    ap.add_argument("--speed", type=float, default=1.0, help="1 = real time pacing")
    ap.add_argument("--extra", help='extra request params JSON, e.g. \'{"end_window_size":800}\'')
    ap.add_argument("--quiet", action="store_true", help="do not print raw responses")
    ap.add_argument("--words", action="store_true", help="keep per-word timing in raw output")
    args = ap.parse_args()
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    if args.env:
        load_env_file(args.env)
    pcm = read_pcm(args.wav)
    ref = normalize(Path(args.ref).read_text(encoding="utf-8")) if args.ref else None

    runs = [("adpcm" if args.adpcm else "pcm", adpcm_roundtrip(pcm) if args.adpcm else pcm)]
    if args.compare:
        runs = [("pcm", pcm), ("adpcm", adpcm_roundtrip(pcm))]
    results = {}
    for label, audio in runs:
        results[label] = asyncio.run(run_once(label, audio, args))

    if len(results) == 2:
        texts = {k: normalize("".join(f.text for f in v)) for k, v in results.items()}
        d = edit_distance(texts["pcm"], texts["adpcm"])
        print(f"\n=== compare: chars pcm={len(texts['pcm'])} adpcm={len(texts['adpcm'])} "
              f"edit distance={d} ({100 * d / max(len(texts['pcm']), 1):.2f}% of pcm)")
        if ref:
            for k, t in texts.items():
                print(f"  CER[{k}] = {100 * edit_distance(ref, t) / max(len(ref), 1):.2f}%")
        for k, v in results.items():
            print(f"  speakers[{k}] = {[f.speaker for f in v]}")
    elif ref:
        (label, finals), = results.items()
        t = normalize("".join(f.text for f in finals))
        print(f"  CER[{label}] = {100 * edit_distance(ref, t) / max(len(ref), 1):.2f}%")
    return 0


if __name__ == "__main__":
    sys.exit(main())
