import asyncio
import itertools
import logging
import os
import shutil
import time
from pathlib import Path
from urllib.parse import quote

import pytest

from app import transcode
from conftest import collect_until, pair, tone_frames

START = {"type": "start", "sample_rate": 16000, "ch": 1, "codec": "ima_adpcm", "fw": "0.1.0"}

needs_ffmpeg = pytest.mark.skipif(shutil.which("ffmpeg") is None, reason="ffmpeg not installed")
_dev = itertools.count()


def record(client, mode: str = "meeting", frames: int = 12) -> str:
    token = pair(client, f"dev-{next(_dev)}")
    with client.websocket_connect("/device/ws",
                                  headers={"Authorization": f"Bearer {token}"}) as ws:
        ws.send_json({**START, "mode": mode})
        rec_id = ws.receive_json()["recording_id"]
        for f in tone_frames(frames):
            ws.send_bytes(f)
        ws.send_json({"type": "stop"})
        collect_until(ws, "saved")
    return rec_id


def wait_background(client, timeout: float = 30) -> None:
    """Wait for the post-recording m4a jobs (they run on the TestClient's loop)."""
    tasks = client.app.state.gateway.m4a_tasks
    deadline = time.monotonic() + timeout
    while tasks and time.monotonic() < deadline:
        time.sleep(0.02)
    assert not tasks


def paths(client, rec_id: str) -> tuple[Path, Path]:
    wav = Path(client.app.state.settings.audio_dir) / f"{rec_id}.wav"
    return wav, transcode.m4a_path(wav)


@pytest.fixture
def fake_ffmpeg(monkeypatch):
    """Replace the ffmpeg call: counts invocations and writes a dummy file."""
    calls = []

    async def fake(wav: Path, out: Path) -> None:
        calls.append(Path(wav))
        await asyncio.sleep(0.05)
        out.write_bytes(b"fake-m4a")

    monkeypatch.setattr(transcode, "_ffmpeg", fake)
    return calls


@needs_ffmpeg
def test_m4a_real_transcode(client):
    rec = record(client, frames=50)  # 2 s of tone
    wait_background(client)
    wav, m4a = paths(client, rec)
    assert m4a.is_file(), "generated in the background after the meeting finished"
    assert 0 < m4a.stat().st_size < wav.stat().st_size

    client.patch(f"/api/recordings/{rec}", json={"title": "周会/记录"})
    r = client.get(f"/api/recordings/{rec}/audio.m4a")
    assert r.status_code == 200
    assert r.headers["content-type"] == "audio/mp4"
    assert r.content[4:8] == b"ftyp"
    disp = r.headers["content-disposition"]
    assert disp.startswith("attachment") and f"filename=\"{rec}.m4a\"" in disp
    assert f"filename*=UTF-8''{quote('周会_记录.m4a')}" in disp

    m4a.unlink()  # cache is rebuilt on demand
    r = client.get(f"/api/recordings/{rec}/audio.m4a")
    assert r.status_code == 200 and r.content[4:8] == b"ftyp" and m4a.is_file()
    assert not list(m4a.parent.glob("*.tmp"))


def test_m4a_cached_and_regenerated_when_stale(client, fake_ffmpeg):
    rec = record(client)
    wait_background(client)
    wav, m4a = paths(client, rec)
    assert fake_ffmpeg == [wav] and m4a.read_bytes() == b"fake-m4a"

    for _ in range(2):
        r = client.get(f"/api/recordings/{rec}/audio.m4a")
        assert r.status_code == 200 and r.content == b"fake-m4a"
    assert len(fake_ffmpeg) == 1  # served from cache

    newer = m4a.stat().st_mtime + 10  # e.g. the WAV grew after a resume
    os.utime(wav, (newer, newer))
    assert client.get(f"/api/recordings/{rec}/audio.m4a").status_code == 200
    assert len(fake_ffmpeg) == 2


def test_concurrent_requests_transcode_once(tmp_path, fake_ffmpeg):
    wav = tmp_path / "a.wav"
    wav.write_bytes(b"RIFF" + bytes(100))

    async def main():
        return await asyncio.gather(*(transcode.ensure_m4a(wav) for _ in range(5)))

    results = asyncio.run(main())
    assert results == [tmp_path / "a.m4a"] * 5
    assert len(fake_ffmpeg) == 1
    assert transcode._locks == {}


def test_m4a_status_codes(client, fake_ffmpeg):
    assert client.get("/api/recordings/nope/audio.m4a").status_code == 404
    note = record(client, mode="note")
    empty = record(client, frames=0)
    wait_background(client)
    assert fake_ffmpeg == []  # neither notes nor empty meetings are transcoded
    assert client.get(f"/api/recordings/{note}/audio.m4a").status_code == 404
    assert client.get(f"/api/recordings/{empty}/audio.m4a").status_code == 404

    token = pair(client)
    with client.websocket_connect("/device/ws",
                                  headers={"Authorization": f"Bearer {token}"}) as ws:
        ws.send_json(START)
        rec_id = ws.receive_json()["recording_id"]
        for f in tone_frames(3):
            ws.send_bytes(f)
        r = client.get(f"/api/recordings/{rec_id}/audio.m4a")
        assert r.status_code == 409 and r.json()["detail"] == "recording in progress"
        ws.send_json({"type": "stop"})
        collect_until(ws, "saved")


def test_delete_removes_m4a(client, fake_ffmpeg):
    rec = record(client)
    wait_background(client)
    wav, m4a = paths(client, rec)
    assert wav.is_file() and m4a.is_file()
    assert client.delete(f"/api/recordings/{rec}").json() == {"ok": True}
    assert not wav.exists() and not m4a.exists()


def test_ffmpeg_missing(client, monkeypatch, caplog):
    monkeypatch.setattr(transcode.shutil, "which", lambda name: None)
    with caplog.at_level(logging.ERROR):
        rec = record(client)  # background job fails, finishing is unaffected
        wait_background(client)
        assert client.get(f"/api/recordings/{rec}").json()["status"] == "done"
        r = client.get(f"/api/recordings/{rec}/audio.m4a")
    assert r.status_code == 500 and r.json()["detail"] == "transcode failed"
    assert any("m4a transcode failed" in m and rec in m for m in caplog.messages)
    wav, m4a = paths(client, rec)
    assert wav.stat().st_size == 44 + 12 * 3200 * 2 and not m4a.exists()
    assert client.get(f"/api/recordings/{rec}/audio").status_code == 200


def test_ffmpeg_start_failure_is_transcode_error(tmp_path, monkeypatch):
    wav = tmp_path / "a.wav"
    wav.write_bytes(b"RIFF" + bytes(100))

    async def boom(*args, **kwargs):
        raise NotImplementedError  # e.g. event loop without subprocess support

    monkeypatch.setattr(transcode.shutil, "which", lambda name: "ffmpeg")
    monkeypatch.setattr(transcode.asyncio, "create_subprocess_exec", boom)
    with pytest.raises(transcode.TranscodeError):
        asyncio.run(transcode.ensure_m4a(wav))
    assert not list(tmp_path.glob("*.m4a*")) and transcode._locks == {}
