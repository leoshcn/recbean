"""Simulate a recbean device: pair, stream a WAV as IMA-ADPCM frames, print events.

    py -3 tools/fake_device.py --host 192.168.2.144:8090 --wav samples/two_speakers.wav
    py -3 tools/fake_device.py --host 127.0.0.1:8080 --auto-confirm          # 5 s test tone
    py -3 tools/fake_device.py --wav x.wav --drop-at 4 --drop-for 3            # test resume

Pairing: without a saved token the tool requests a 4-digit code and waits until it is
confirmed on the Web page (配对 page), or confirms it itself with --auto-confirm.
Tokens are cached in --token-file (default ~/.recbean_fake_device.json).
The WAV must be 16 kHz / mono / 16-bit (convert: ffmpeg -i in -ar 16000 -ac 1 out.wav).
Exit code: 0 saved, 2 saved but ASR error reported, 1 failure.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import math
import sys
import time
import urllib.error
import urllib.request
import wave
from array import array
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from app.adpcm import FRAME_SAMPLES, iter_frames  # noqa: E402


def http_json(method: str, url: str, body: dict | None = None) -> tuple[int, dict]:
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method,
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"{}")
        except json.JSONDecodeError:
            return e.code, {}


def load_tokens(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}


def save_token(path: Path, key: str, token: str | None) -> None:
    tokens = load_tokens(path)
    if token is None:
        tokens.pop(key, None)
    else:
        tokens[key] = token
    path.write_text(json.dumps(tokens, indent=2), encoding="utf-8")


def pair(base: str, device_id: str, auto_confirm: bool, timeout: float = 600) -> str:
    status, body = http_json("POST", f"{base}/device/pair", {"device_id": device_id})
    if status != 200:
        raise SystemExit(f"pair request failed: HTTP {status} {body}")
    code = body["code"]
    print(f"[pair] code {code} for device {device_id}")
    if auto_confirm:
        status, body = http_json("POST", f"{base}/api/pairings", {"code": code, "name": "fake"})
        print(f"[pair] auto-confirm -> HTTP {status} {body}")
    else:
        print(f"[pair] confirm it at {base}/#/pairing")
    deadline = time.time() + timeout
    while time.time() < deadline:
        status, body = http_json("GET", f"{base}/device/pair/{device_id}")
        if body.get("status") == "paired":
            print("[pair] paired")
            return body["token"]
        if status == 404:
            raise SystemExit("pairing expired")
        time.sleep(1)
    raise SystemExit("pairing timeout")


def read_wav(path: str | None, seconds: float) -> bytes:
    if not path:
        n = int(16000 * seconds)
        return array("h", (int(6000 * math.sin(2 * math.pi * 440 * i / 16000))
                           for i in range(n))).tobytes()
    with wave.open(path) as w:
        if (w.getframerate(), w.getnchannels(), w.getsampwidth()) != (16000, 1, 2):
            raise SystemExit(f"{path}: need 16 kHz mono 16-bit, got {w.getframerate()} Hz "
                             f"{w.getnchannels()} ch {8 * w.getsampwidth()} bit "
                             "(ffmpeg -i in.wav -ar 16000 -ac 1 -sample_fmt s16 out.wav)")
        return w.readframes(w.getnframes())


class Stats:
    def __init__(self) -> None:
        self.finals: list[dict] = []
        self.saved: dict | None = None
        self.asr_error = False
        self.auth_error = False
        self.started: dict | None = None
        self.event = asyncio.Event()


async def receiver(ws, stats: Stats, verbose: bool) -> None:
    from websockets.exceptions import ConnectionClosed

    try:
        async for raw in ws:
            msg = json.loads(raw)
            t = msg.get("type")
            if t == "partial":
                if verbose:
                    print(f"  … {msg['text']}")
            elif t == "final":
                stats.finals.append(msg)
                print(f"  [{msg['start_ms'] / 1000:7.1f}s] 说话人{msg['speaker']}: {msg['text']}")
            elif t == "started":
                stats.started = msg
                print(f"[ws] started {msg}")
            elif t == "saved":
                stats.saved = msg
                print(f"[ws] saved {msg}")
            elif t == "error":
                print(f"[ws] error {msg}")
                if msg.get("code") == "asr_error":
                    stats.asr_error = True
                if msg.get("code") == "auth":
                    stats.auth_error = True
            else:
                print(f"[ws] {msg}")
            stats.event.set()
    except ConnectionClosed:
        pass
    stats.event.set()


async def wait_for(stats: Stats, cond, timeout: float) -> bool:
    deadline = time.monotonic() + timeout
    while not cond():
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return False
        stats.event.clear()
        try:
            await asyncio.wait_for(stats.event.wait(), remaining)
        except asyncio.TimeoutError:
            return cond()
    return True


async def stream(args, ws_url: str, token: str, frames: list[bytes]) -> int:
    from websockets.asyncio.client import connect

    headers = {"Authorization": f"Bearer {token}"}
    stats = Stats()
    interval = 0.2 / args.speed if args.speed > 0 else 0
    ws = await connect(ws_url, additional_headers=headers, max_size=None)
    rx = asyncio.create_task(receiver(ws, stats, args.verbose))
    await ws.send(json.dumps({"type": "start", "sample_rate": 16000, "ch": 1,
                              "codec": "ima_adpcm", "fw": "fake-0.1"}))
    if not await wait_for(stats, lambda: stats.started or stats.auth_error, 10):
        print("[ws] no 'started' from server")
        return 1
    if stats.auth_error:
        return 3
    rec_id = stats.started["recording_id"]
    drop_frame = int(args.drop_at * 5) if args.drop_at is not None else -1
    t0 = time.monotonic()
    i = 0
    while i < len(frames):
        if i == drop_frame:
            print(f"[sim] dropping connection for {args.drop_for}s at frame {i}")
            await ws.close()
            await rx
            await asyncio.sleep(args.drop_for)
            ws = await connect(ws_url, additional_headers=headers, max_size=None)
            stats.started = None
            rx = asyncio.create_task(receiver(ws, stats, args.verbose))
            resend_from = max(0, i - 5)  # device resends its unacked buffer
            await ws.send(json.dumps({"type": "resume", "recording_id": rec_id,
                                      "next_seq": resend_from}))
            if not await wait_for(stats, lambda: stats.started or stats.saved, 10):
                print("[ws] resume not acknowledged")
                return 1
            if stats.saved:
                print("[sim] server already finalized the recording")
                break
            for f in frames[resend_from:i]:
                await ws.send(f)
            t0 = time.monotonic() - i * interval
            drop_frame = -1
        await ws.send(frames[i])
        i += 1
        delay = t0 + i * interval - time.monotonic()
        if delay > 0:
            await asyncio.sleep(delay)
    if not stats.saved:
        await ws.send(json.dumps({"type": "stop"}))
        await wait_for(stats, lambda: stats.saved, args.stop_timeout)
    await ws.close()
    await rx
    if not stats.saved:
        print("[ws] no 'saved' from server")
        return 1
    print(f"[done] recording {stats.saved['recording_id']}: "
          f"{stats.saved['duration_ms'] / 1000:.1f}s, {stats.saved['sentences']} sentences, "
          f"speakers seen {sorted({f['speaker'] for f in stats.finals})}")
    return 2 if stats.asr_error else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1:8080", help="server host:port")
    ap.add_argument("--wav", help="16 kHz mono s16 WAV (default: 5 s test tone)")
    ap.add_argument("--seconds", type=float, default=5, help="tone length without --wav")
    ap.add_argument("--device-id", default="fake-device-001")
    ap.add_argument("--token-file", default=str(Path.home() / ".recbean_fake_device.json"))
    ap.add_argument("--auto-confirm", action="store_true", help="confirm pairing via the API")
    ap.add_argument("--speed", type=float, default=1.0, help="1 = real time, 0 = no pacing")
    ap.add_argument("--drop-at", type=float, help="simulate disconnect at this second")
    ap.add_argument("--drop-for", type=float, default=3.0, help="disconnect duration (s)")
    ap.add_argument("--stop-timeout", type=float, default=30.0)
    ap.add_argument("-v", "--verbose", action="store_true", help="print partial results")
    args = ap.parse_args()
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    base = f"http://{args.host}"
    ws_url = f"ws://{args.host}/device/ws"
    token_file = Path(args.token_file)
    key = f"{args.host}/{args.device_id}"
    frames = list(iter_frames(read_wav(args.wav, args.seconds)))
    print(f"[audio] {len(frames)} frames ({len(frames) * FRAME_SAMPLES / 16000:.1f}s)")

    for attempt in range(2):
        token = load_tokens(token_file).get(key)
        if not token:
            token = pair(base, args.device_id, args.auto_confirm)
            save_token(token_file, key, token)
        rc = asyncio.run(stream(args, ws_url, token, frames))
        if rc != 3:
            return rc
        print("[ws] token rejected, re-pairing")
        save_token(token_file, key, None)
    return 1


if __name__ == "__main__":
    sys.exit(main())
