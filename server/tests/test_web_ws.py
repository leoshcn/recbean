"""Browser recording WebSocket ``/web/ws`` (task 10-20-web-browser-recording)."""

import math
import struct
import time
import wave
from array import array
from pathlib import Path

from conftest import FakeAsr, collect_until, pair, tone_frames

START = {"type": "start", "codec": "pcm_s16le", "sample_rate": 16000, "ch": 1,
         "client": "web/0.1.0"}


def pcm_chunks(n: int, freq: float = 440.0) -> list[bytes]:
    """n x 200 ms of 16 kHz s16le mono tone."""
    out = []
    for k in range(n):
        samples = array("h", (
            int(8000 * math.sin(2 * math.pi * freq * (k * 3200 + i) / 16000))
            for i in range(3200)))
        out.append(samples.tobytes())
    return out


def frame(seq: int, pcm: bytes) -> bytes:
    return struct.pack("<I", seq) + pcm


def wav_bytes(client, rec_id) -> bytes:
    path = Path(client.app.state.settings.audio_dir) / f"{rec_id}.wav"
    with wave.open(str(path)) as w:
        assert (w.getframerate(), w.getnchannels(), w.getsampwidth()) == (16000, 1, 2)
        return w.readframes(w.getnframes())


def wait_status(client, rec_id, status, timeout=5.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        rec = client.get(f"/api/recordings/{rec_id}").json()
        if rec["status"] == status:
            return rec
        time.sleep(0.05)
    raise AssertionError(f"status never became {status}: {rec}")


def wait_detached(client, rec_id):
    session = client.app.state.gateway.sessions[rec_id]
    deadline = time.time() + 5
    while session.conn is not None and time.time() < deadline:
        time.sleep(0.02)
    assert session.conn is None and session.resume_timer is not None
    return session


def test_record_end_to_end(client):
    chunks = pcm_chunks(12)
    with client.websocket_connect("/web/ws") as ws:
        ws.send_json(START)
        started = ws.receive_json()
        assert started["type"] == "started" and "resumed" not in started
        assert len(started["resume_token"]) >= 20
        rec_id = started["recording_id"]
        live = client.get(f"/api/recordings/{rec_id}").json()
        assert live["live"] is True and live["status"] == "recording"
        for seq, pcm in enumerate(chunks):
            ws.send_bytes(frame(seq, pcm))
        ws.send_json({"type": "stop"})
        msgs = collect_until(ws, "saved")
    assert msgs[-1] == {"type": "saved", "recording_id": rec_id, "duration_ms": 2400,
                        "sentences": 3}
    finals = [m for m in msgs if m["type"] == "final"]
    assert [(m["idx"], m["speaker"], m["text"]) for m in finals] == [
        (0, 1, "第1句。"), (1, 2, "第2句。"), (2, 1, "第3句。")]
    assert any(m["type"] == "partial" for m in msgs)
    assert FakeAsr.calls[-1] == ("meeting", [])

    rec = client.get(f"/api/recordings/{rec_id}").json()
    assert rec["device_id"] == "browser" and rec["kind"] == "meeting"
    assert rec["title"].startswith("会议 ") and rec["status"] == "done"
    assert rec["duration_ms"] == 2400 and rec["live"] is False
    assert [r["id"] for r in client.get("/api/recordings?kind=meeting").json()] == [rec_id]
    assert wav_bytes(client, rec_id) == b"".join(chunks)


def test_bad_start_busy_and_stop_without_recording(client):
    with client.websocket_connect("/web/ws") as ws:
        ws.send_json({"type": "stop"})
        assert ws.receive_json()["code"] == "not_recording"
        for bad in ({**START, "codec": "ima_adpcm"}, {**START, "sample_rate": 48000},
                    {**START, "ch": 2}, {"type": "start"}):
            ws.send_json(bad)
            assert ws.receive_json()["code"] == "bad_request"
        ws.send_json({**START, "mode": "note"})  # mode ignored: always meeting
        rec_id = ws.receive_json()["recording_id"]
        ws.send_json(START)
        assert ws.receive_json() == {"type": "error", "code": "busy",
                                     "msg": "recording already in progress"}
        ws.send_json({"type": "ping"})
        assert ws.receive_json() == {"type": "pong"}
        ws.send_json({"type": "stop"})
        collect_until(ws, "saved")
    assert client.get(f"/api/recordings/{rec_id}").json()["kind"] == "meeting"
    assert len(client.get("/api/recordings").json()) == 1


def test_concurrent_browser_recordings(client):
    with client.websocket_connect("/web/ws") as a, client.websocket_connect("/web/ws") as b:
        a.send_json(START)
        id_a = a.receive_json()["recording_id"]
        b.send_json(START)
        id_b = b.receive_json()["recording_id"]
        assert id_a != id_b
        # a second browser start does not abandon the first one
        assert {id_a, id_b} <= set(client.app.state.gateway.sessions)
        for ws in (a, b):
            ws.send_bytes(frame(0, pcm_chunks(1)[0]))
            ws.send_json({"type": "stop"})
            assert collect_until(ws, "saved")[-1]["duration_ms"] == 200


def test_bad_frames_dropped_and_gap_filled(client):
    chunks = pcm_chunks(5)
    with client.websocket_connect("/web/ws") as ws:
        ws.send_json(START)
        rec_id = ws.receive_json()["recording_id"]
        ws.send_bytes(frame(0, chunks[0]))
        ws.send_bytes(frame(1, chunks[1][:-2]))  # short frame dropped
        ws.send_bytes(frame(1, chunks[1] + b"\0\0"))  # long frame dropped
        ws.send_bytes(b"\x01\x02")
        ws.send_bytes(frame(1, chunks[1]))
        ws.send_bytes(frame(4, chunks[4]))  # 2, 3 missing -> silence
        ws.send_json({"type": "stop"})
        assert collect_until(ws, "saved")[-1]["duration_ms"] == 1000
    assert wav_bytes(client, rec_id) == chunks[0] + chunks[1] + bytes(2 * 6400) + chunks[4]


def test_resume_dedupes_resent_frames(client):
    chunks = pcm_chunks(10)
    with client.websocket_connect("/web/ws") as ws:
        ws.send_json(START)
        started = ws.receive_json()
        rec_id, token = started["recording_id"], started["resume_token"]
        for seq in range(5):
            ws.send_bytes(frame(seq, chunks[seq]))
    session = wait_detached(client, rec_id)
    marker = {"type": "final", "idx": 99, "speaker": 1, "start_ms": 0, "end_ms": 1,
              "text": "离线期间"}
    client.portal.call(session.push, marker)
    with client.websocket_connect("/web/ws") as ws:
        # wrong / missing token is refused and does not steal the session
        ws.send_json({"type": "resume", "recording_id": rec_id, "resume_token": "x" * 32,
                      "next_seq": 3})
        assert ws.receive_json()["code"] == "resume_failed"
        ws.send_json({"type": "resume", "recording_id": rec_id, "next_seq": 3})
        assert ws.receive_json()["code"] == "resume_failed"
        assert session.conn is None
        # browser resends from its ring buffer (3, 4 are duplicates)
        ws.send_json({"type": "resume", "recording_id": rec_id, "resume_token": token,
                      "next_seq": 3})
        assert ws.receive_json() == {"type": "started", "recording_id": rec_id,
                                     "resumed": True, "next_seq": 5}
        flushed = [ws.receive_json()]
        if flushed[0] != marker:  # ASR final 0 may have been buffered too
            flushed.append(ws.receive_json())
        assert flushed[-1] == marker
        for seq in range(3, 10):
            ws.send_bytes(frame(seq, chunks[seq]))
        ws.send_json({"type": "stop"})
        msgs = collect_until(ws, "saved")
    assert msgs[-1]["duration_ms"] == 2000
    assert wav_bytes(client, rec_id) == b"".join(chunks)
    assert client.get(f"/api/recordings/{rec_id}").json()["status"] == "done"
    assert len(client.get("/api/recordings").json()) == 1
    # resume after the recording finished -> saved (only with the right token)
    with client.websocket_connect("/web/ws") as ws:
        ws.send_json({"type": "resume", "recording_id": rec_id, "resume_token": token,
                      "next_seq": 10})
        assert ws.receive_json() == {"type": "saved", "recording_id": rec_id,
                                     "duration_ms": 2000, "sentences": 2}
        ws.send_json({"type": "resume", "recording_id": rec_id, "resume_token": "bad",
                      "next_seq": 10})
        assert ws.receive_json()["code"] == "resume_failed"


def test_disconnect_without_resume_marks_interrupted(make_client):
    client = make_client(resume_timeout_s=0.3)
    with client.websocket_connect("/web/ws") as ws:
        ws.send_json(START)
        started = ws.receive_json()
        rec_id, token = started["recording_id"], started["resume_token"]
        for seq, pcm in enumerate(pcm_chunks(7)):
            ws.send_bytes(frame(seq, pcm))
    rec = wait_status(client, rec_id, "interrupted")
    assert rec["duration_ms"] == 1400 and len(rec["sentences"]) == 2
    assert rec["live"] is False
    with client.websocket_connect("/web/ws") as ws:
        ws.send_json({"type": "resume", "recording_id": rec_id, "resume_token": token,
                      "next_seq": 7})
        assert ws.receive_json() == {"type": "saved", "recording_id": rec_id,
                                     "duration_ms": 1400, "sentences": 2}
        ws.send_json({"type": "resume", "recording_id": "nope", "resume_token": token,
                      "next_seq": 0})
        assert ws.receive_json()["code"] == "resume_failed"


def test_asr_error_keeps_audio(make_client):
    client = make_client(asr_factory=lambda kind, hw: FakeAsr(kind, hw, fail_after=2))
    with client.websocket_connect("/web/ws") as ws:
        ws.send_json(START)
        rec_id = ws.receive_json()["recording_id"]
        for seq, pcm in enumerate(pcm_chunks(5)):
            ws.send_bytes(frame(seq, pcm))
        ws.send_json({"type": "stop"})
        msgs = collect_until(ws, "saved")
    assert [m["code"] for m in msgs if m["type"] == "error"] == ["asr_error"]
    assert msgs[-1]["duration_ms"] == 1000
    assert client.get(f"/api/recordings/{rec_id}").json()["status"] == "asr_error"


def test_browser_and_device_cannot_resume_each_other(client):
    token = pair(client)
    auth = {"Authorization": f"Bearer {token}"}
    with client.websocket_connect("/device/ws", headers=auth) as dev:
        dev.send_json({"type": "start", "sample_rate": 16000, "ch": 1, "codec": "ima_adpcm"})
        dev_rec = dev.receive_json()["recording_id"]
        dev.send_bytes(tone_frames(1)[0])
        with client.websocket_connect("/web/ws") as web:
            web.send_json(START)
            started = web.receive_json()
            web_rec, web_token = started["recording_id"], started["resume_token"]
            # browser start did not abandon the device recording
            assert dev_rec in client.app.state.gateway.sessions
            with client.websocket_connect("/web/ws") as other:
                other.send_json({"type": "resume", "recording_id": dev_rec,
                                 "resume_token": web_token, "next_seq": 1})
                assert other.receive_json()["code"] == "resume_failed"
            web.send_json({"type": "stop"})
            collect_until(web, "saved")
        dev.send_json({"type": "stop"})
        collect_until(dev, "saved")
        dev.send_json({"type": "resume", "recording_id": web_rec, "next_seq": 0})
        assert dev.receive_json()["code"] == "resume_failed"
        # browser codec is not accepted on the device endpoint (unchanged)
        dev.send_json(START)
        assert dev.receive_json()["code"] == "bad_request"
    recs = {r["id"]: r for r in client.get("/api/recordings").json()}
    assert recs[dev_rec]["device_id"] == "dev-test" and recs[dev_rec]["status"] == "done"
    assert recs[web_rec]["device_id"] == "browser"


def test_failed_resume_keeps_active_recording(make_client):
    client = make_client(resume_timeout_s=0.3)
    chunks = pcm_chunks(3)
    with client.websocket_connect("/web/ws") as ws:
        ws.send_json(START)
        rec_id = ws.receive_json()["recording_id"]
        ws.send_bytes(frame(0, chunks[0]))
        ws.send_json({"type": "resume", "recording_id": rec_id, "resume_token": "bad",
                      "next_seq": 0})
        assert collect_until(ws, "error")[-1]["code"] == "resume_failed"
        ws.send_bytes(frame(1, chunks[1]))
        ws.send_bytes(frame(2, chunks[2]))
        ws.send_json({"type": "stop"})
        while (m := ws.receive_json())["type"] not in ("saved", "error"):
            pass  # partial / final
        assert m["type"] == "saved" and m["duration_ms"] == 600, m
    assert wav_bytes(client, rec_id) == b"".join(chunks)
