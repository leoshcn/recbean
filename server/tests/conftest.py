from __future__ import annotations

import math
import sys
from array import array
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from app.adpcm import AdpcmEncoder, FRAME_SAMPLES  # noqa: E402
from app.asr import AsrError, FinalEvent, PartialEvent  # noqa: E402
from app.config import Settings  # noqa: E402
from app.main import create_app  # noqa: E402

FIXTURES = Path(__file__).parent / "fixtures"


class FakeAsr:
    """Emits a partial per chunk and a final every 5 chunks; speakers alternate.

    Also usable directly as an ``AsrFactory``; every instance records the
    ``(kind, hotwords)`` it was created with in ``FakeAsr.calls``.
    """

    calls: list = []

    def __init__(self, kind: str = "meeting", hotwords=None,
                 fail_after: int | None = None) -> None:
        self.kind = kind
        self.hotwords = list(hotwords or [])
        self.fail_after = fail_after
        FakeAsr.calls.append((kind, self.hotwords))

    async def run(self, audio, emit):
        n = 0
        ms = 0
        seg_start = 0
        sentence = 0
        async for chunk in audio:
            n += 1
            ms += len(chunk) // 32
            if self.fail_after is not None and n >= self.fail_after:
                raise AsrError("fake asr failure")
            await emit(PartialEvent(f"进行中{n}"))
            if n % 5 == 0:
                sentence += 1
                await emit(FinalEvent(f"第{sentence}句。", seg_start, ms, str(sentence % 2)))
                seg_start = ms
        if ms > seg_start:
            sentence += 1
            await emit(FinalEvent(f"第{sentence}句。", seg_start, ms, str(sentence % 2)))
        await emit(PartialEvent(""))


def make_settings(tmp_path: Path, **kw) -> Settings:
    s = Settings()
    s.data_dir = tmp_path / "data"
    s.web_dist = tmp_path / "no-web"
    s.doubao_app_key = s.doubao_access_key = s.doubao_api_key = ""
    s.resume_timeout_s = kw.get("resume_timeout_s", 30)
    s.asr_finish_timeout_s = 5
    s.timezone = kw.get("timezone", "Asia/Shanghai")
    s.hotword_token_budget = kw.get("hotword_token_budget", 100)
    return s


@pytest.fixture
def make_client(tmp_path):
    clients = []

    FakeAsr.calls.clear()

    def factory(asr_factory=FakeAsr, **kw):
        app = create_app(make_settings(tmp_path, **kw), asr_factory=asr_factory)
        c = TestClient(app)
        c.__enter__()
        clients.append(c)
        return c

    yield factory
    for c in clients:
        c.__exit__(None, None, None)


@pytest.fixture
def client(make_client):
    return make_client()


def pair(client: TestClient, device_id: str = "dev-test") -> str:
    code = client.post("/device/pair", json={"device_id": device_id}).json()["code"]
    assert client.post("/api/pairings", json={"code": code}).status_code == 200
    body = client.get(f"/device/pair/{device_id}").json()
    assert body["status"] == "paired"
    return body["token"]


def tone_frames(n: int, freq: float = 440.0) -> list[bytes]:
    enc = AdpcmEncoder()
    frames = []
    for k in range(n):
        samples = array("h", (
            int(8000 * math.sin(2 * math.pi * freq * (k * FRAME_SAMPLES + i) / 16000))
            for i in range(FRAME_SAMPLES)))
        frames.append(enc.encode_frame(samples))
    return frames


def collect_until(ws, kind: str, limit: int = 500) -> list[dict]:
    msgs = []
    for _ in range(limit):
        m = ws.receive_json()
        msgs.append(m)
        if m["type"] == kind:
            return msgs
    raise AssertionError(f"no {kind!r} in {msgs}")
