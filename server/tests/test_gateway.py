import time
import wave
from pathlib import Path

import pytest
from starlette.websockets import WebSocketDisconnect

from app.adpcm import decode_frame
from app.asr import UnconfiguredAsrEngine
from conftest import FakeAsr, collect_until, pair, tone_frames

START = {"type": "start", "sample_rate": 16000, "ch": 1, "codec": "ima_adpcm", "fw": "0.1.0"}


def auth(token):
    return {"Authorization": f"Bearer {token}"}


def wait_status(client, rec_id, status, timeout=5.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        rec = client.get(f"/api/recordings/{rec_id}").json()
        if rec["status"] == status:
            return rec
        time.sleep(0.05)
    raise AssertionError(f"status never became {status}: {rec}")


def test_pairing_flow(client):
    r = client.post("/device/pair", json={"device_id": "dev-a"})
    code = r.json()["code"]
    assert len(code) == 4 and code.isdigit()
    # same device asking again keeps its code
    assert client.post("/device/pair", json={"device_id": "dev-a"}).json()["code"] == code
    assert client.get("/device/pair/dev-a").json() == {"status": "pending", "code": code}
    pend = client.get("/api/pairings").json()
    assert [p["device_id"] for p in pend] == ["dev-a"]
    bad = "0000" if code != "0000" else "1111"
    assert client.post("/api/pairings", json={"code": bad}).status_code == 404
    assert client.post("/api/pairings", json={"code": code, "name": "测试豆"}).status_code == 200
    assert client.get("/api/pairings").json() == []
    got = client.get("/device/pair/dev-a").json()
    assert got["status"] == "paired" and len(got["token"]) > 20
    # token handed out once
    assert client.get("/device/pair/dev-a").status_code == 404
    devs = client.get("/api/devices").json()
    assert devs[0]["id"] == "dev-a" and devs[0]["name"] == "测试豆"
    assert "token_hash" not in devs[0]


def test_unpaired_device_rejected(client):
    with client.websocket_connect("/device/ws", headers=auth("nope")) as ws:
        msg = ws.receive_json()
        assert msg == {"type": "error", "code": "auth", "msg": "device not paired"}
        with pytest.raises(WebSocketDisconnect):
            ws.receive_json()
    with client.websocket_connect("/device/ws") as ws:
        assert ws.receive_json()["code"] == "auth"


def test_record_stream_end_to_end(client):
    token = pair(client)
    frames = tone_frames(12)
    with client.websocket_connect("/device/ws", headers=auth(token)) as ws:
        ws.send_json(START)
        started = ws.receive_json()
        assert started["type"] == "started"
        rec_id = started["recording_id"]
        assert client.get("/api/recordings").json()[0]["status"] == "recording"
        for f in frames:
            ws.send_bytes(f)
        ws.send_json({"type": "stop"})
        msgs = collect_until(ws, "saved")
    saved = msgs[-1]
    assert saved == {"type": "saved", "recording_id": rec_id, "duration_ms": 2400,
                     "sentences": 3}
    finals = [m for m in msgs if m["type"] == "final"]
    assert [(m["idx"], m["speaker"], m["text"]) for m in finals] == [
        (0, 1, "第1句。"), (1, 2, "第2句。"), (2, 1, "第3句。")]
    assert finals[0]["start_ms"] == 0 and finals[0]["end_ms"] == 1000
    assert any(m["type"] == "partial" for m in msgs)

    rec = client.get("/api/recordings").json()[0]
    assert rec["id"] == rec_id and rec["status"] == "done"
    assert rec["duration_ms"] == 2400 and rec["speaker_count"] == 2
    assert rec["first_sentence"] == "第1句。" and rec["live"] is False
    detail = client.get(f"/api/recordings/{rec_id}").json()
    assert [s["name"] for s in detail["speakers"]] == ["说话人1", "说话人2"]

    # WAV on disk equals decoded ADPCM
    path = Path(client.app.state.settings.audio_dir) / f"{rec_id}.wav"
    with wave.open(str(path)) as w:
        assert (w.getframerate(), w.getnchannels(), w.getsampwidth()) == (16000, 1, 2)
        assert w.getnframes() == 12 * 3200
        assert w.readframes(w.getnframes()) == b"".join(decode_frame(f)[1] for f in frames)


def test_start_twice_is_busy_and_stop_without_recording(client):
    token = pair(client)
    with client.websocket_connect("/device/ws", headers=auth(token)) as ws:
        ws.send_json({"type": "stop"})
        assert ws.receive_json()["code"] == "not_recording"
        ws.send_json({**START, "codec": "pcm"})
        assert ws.receive_json()["code"] == "bad_request"
        ws.send_text("not json")
        assert ws.receive_json()["code"] == "bad_request"
        ws.send_json(START)
        assert ws.receive_json()["type"] == "started"
        ws.send_json(START)
        assert ws.receive_json() == {"type": "error", "code": "busy",
                                     "msg": "recording already in progress"}
        ws.send_json({"type": "stop"})
        collect_until(ws, "saved")
        # a second recording on the same connection works
        ws.send_json(START)
        assert ws.receive_json()["type"] == "started"
        ws.send_json({"type": "stop"})
        assert collect_until(ws, "saved")[-1]["duration_ms"] == 0
    assert len(client.get("/api/recordings").json()) == 2


def test_resume_dedupes_resent_frames(client):
    token = pair(client)
    frames = tone_frames(10)
    with client.websocket_connect("/device/ws", headers=auth(token)) as ws:
        ws.send_json(START)
        rec_id = ws.receive_json()["recording_id"]
        for f in frames[:5]:
            ws.send_bytes(f)
    # abrupt disconnect; events produced while detached are buffered
    session = client.app.state.gateway.sessions[rec_id]
    deadline = time.time() + 5
    while session.conn is not None and time.time() < deadline:
        time.sleep(0.02)
    assert session.conn is None and session.resume_timer is not None
    marker = {"type": "final", "idx": 99, "speaker": 1, "start_ms": 0, "end_ms": 1,
              "text": "离线期间"}
    client.portal.call(session.push, marker)
    # device reconnects and resends from its buffer (3, 4 are duplicates)
    with client.websocket_connect("/device/ws", headers=auth(token)) as ws:
        ws.send_json({"type": "resume", "recording_id": rec_id, "next_seq": 3})
        resumed = ws.receive_json()
        assert resumed["type"] == "started"
        assert resumed["recording_id"] == rec_id and resumed["resumed"] is True
        assert resumed["next_seq"] == 5
        flushed = [ws.receive_json()]
        if flushed[0] != marker:  # ASR final 0 may have been buffered too
            flushed.append(ws.receive_json())
        assert flushed[-1] == marker
        for f in frames[3:]:
            ws.send_bytes(f)
        ws.send_json({"type": "stop"})
        msgs = collect_until(ws, "saved")
    assert msgs[-1]["duration_ms"] == 2000
    assert 1 in [m["idx"] for m in msgs if m["type"] == "final"]
    path = Path(client.app.state.settings.audio_dir) / f"{rec_id}.wav"
    with wave.open(str(path)) as w:
        assert w.readframes(w.getnframes()) == b"".join(decode_frame(f)[1] for f in frames)
    assert client.get(f"/api/recordings/{rec_id}").json()["status"] == "done"


def test_sequence_gap_filled_with_silence(client):
    token = pair(client)
    frames = tone_frames(5)
    with client.websocket_connect("/device/ws", headers=auth(token)) as ws:
        ws.send_json(START)
        ws.receive_json()
        for f in (frames[0], frames[1], frames[4]):
            ws.send_bytes(f)
        ws.send_bytes(b"\x01\x02")  # garbage frame ignored
        ws.send_json({"type": "stop"})
        assert collect_until(ws, "saved")[-1]["duration_ms"] == 1000


def test_disconnect_without_resume_marks_interrupted(make_client):
    client = make_client(resume_timeout_s=0.3)
    token = pair(client)
    with client.websocket_connect("/device/ws", headers=auth(token)) as ws:
        ws.send_json(START)
        rec_id = ws.receive_json()["recording_id"]
        for f in tone_frames(7):
            ws.send_bytes(f)
    rec = wait_status(client, rec_id, "interrupted")
    assert rec["duration_ms"] == 1400 and len(rec["sentences"]) == 2
    path = Path(client.app.state.settings.audio_dir) / f"{rec_id}.wav"
    with wave.open(str(path)) as w:
        assert w.getnframes() == 7 * 3200
    # a late resume is told the recording was saved
    with client.websocket_connect("/device/ws", headers=auth(token)) as ws:
        ws.send_json({"type": "resume", "recording_id": rec_id, "next_seq": 7})
        assert ws.receive_json() == {"type": "saved", "recording_id": rec_id,
                                     "duration_ms": 1400, "sentences": 2}
        ws.send_json({"type": "resume", "recording_id": "nope", "next_seq": 0})
        assert ws.receive_json()["code"] == "resume_failed"


@pytest.mark.parametrize("factory", [lambda kind, hw: FakeAsr(kind, hw, fail_after=2),
                                     UnconfiguredAsrEngine],
                         ids=["asr_fails_midway", "asr_unconfigured"])
def test_asr_failure_keeps_audio(make_client, factory):
    client = make_client(asr_factory=factory)
    token = pair(client)
    frames = tone_frames(5)
    with client.websocket_connect("/device/ws", headers=auth(token)) as ws:
        ws.send_json(START)
        rec_id = ws.receive_json()["recording_id"]
        for f in frames:
            ws.send_bytes(f)
        ws.send_json({"type": "stop"})
        msgs = collect_until(ws, "saved")
    errors = [m for m in msgs if m["type"] == "error"]
    assert errors and errors[0]["code"] == "asr_error"
    assert msgs[-1]["duration_ms"] == 1000
    rec = client.get(f"/api/recordings/{rec_id}").json()
    assert rec["status"] == "asr_error" and rec["error"]
    path = Path(client.app.state.settings.audio_dir) / f"{rec_id}.wav"
    with wave.open(str(path)) as w:
        assert w.getnframes() == 5 * 3200


def test_restart_recovers_unfinished_recording(tmp_path):
    from fastapi.testclient import TestClient

    from app.main import create_app
    from app.store import Store
    from app.wav import wav_header
    from conftest import make_settings

    settings = make_settings(tmp_path)
    settings.audio_dir.mkdir(parents=True)
    wav_path = settings.audio_dir / "crashed.wav"
    wav_path.write_bytes(wav_header(0) + bytes(600 * 32))  # header never fixed
    store = Store(settings.db_path)
    store.create_recording("crashed", "t", "dev", str(wav_path))
    store.close()
    with TestClient(create_app(settings, asr_factory=FakeAsr)) as client:
        rec = client.get("/api/recordings/crashed").json()
        assert rec["status"] == "interrupted" and rec["duration_ms"] == 600
    with wave.open(str(wav_path)) as w:
        assert w.getnframes() == 600 * 16


def record(client, token, start, n_frames=12):
    with client.websocket_connect("/device/ws", headers=auth(token)) as ws:
        ws.send_json(start)
        rec_id = ws.receive_json()["recording_id"]
        for f in tone_frames(n_frames):
            ws.send_bytes(f)
        ws.send_json({"type": "stop"})
        msgs = collect_until(ws, "saved")
    return rec_id, msgs


@pytest.mark.parametrize("mode", [None, "meeting", "bogus", 1])
def test_start_without_or_unknown_mode_is_meeting(client, mode):
    token = pair(client)
    start = dict(START) if mode is None else {**START, "mode": mode}
    rec_id, msgs = record(client, token, start)
    rec = client.get(f"/api/recordings/{rec_id}").json()
    assert rec["kind"] == "meeting"
    assert rec["title"].startswith("会议 ")
    assert FakeAsr.calls[-1] == ("meeting", [])
    finals = [m for m in msgs if m["type"] == "final"]
    assert [m["speaker"] for m in finals] == [1, 2, 1]  # diarization kept


def test_note_mode_single_speaker_and_title(client):
    token = pair(client)
    rec_id, msgs = record(client, token, {**START, "mode": "note"})
    assert FakeAsr.calls[-1][0] == "note"
    finals = [m for m in msgs if m["type"] == "final"]
    assert len(finals) == 3 and {m["speaker"] for m in finals} == {1}
    rec = client.get(f"/api/recordings/{rec_id}").json()
    assert rec["kind"] == "note" and rec["title"].startswith("笔记 ")
    assert rec["speaker_count"] == 1
    assert [s["idx"] for s in rec["speakers"]] == [1]
    assert {s["speaker_idx"] for s in rec["sentences"]} == {1}


def test_hotwords_snapshot_at_start(client):
    token = pair(client)
    client.post("/api/hotwords", json={"words": ["录音豆", "豆包"]})
    with client.websocket_connect("/device/ws", headers=auth(token)) as ws:
        ws.send_json({**START, "mode": "note"})
        ws.receive_json()
        # edits while recording only affect later recordings
        client.post("/api/hotwords", json={"words": ["火山引擎"]})
        for f in tone_frames(3):
            ws.send_bytes(f)
        ws.send_json({"type": "stop"})
        collect_until(ws, "saved")
    assert FakeAsr.calls[-1] == ("note", ["录音豆", "豆包"])
    record(client, token, START, 3)
    assert FakeAsr.calls[-1] == ("meeting", ["录音豆", "豆包", "火山引擎"])
