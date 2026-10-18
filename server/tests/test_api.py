import sqlite3
from datetime import datetime
from pathlib import Path
from zoneinfo import ZoneInfo

import pytest

from conftest import collect_until, pair, tone_frames

START = {"type": "start", "sample_rate": 16000, "ch": 1, "codec": "ima_adpcm", "fw": "0.1.0"}


@pytest.fixture
def rec(client):
    token = pair(client)
    with client.websocket_connect("/device/ws",
                                  headers={"Authorization": f"Bearer {token}"}) as ws:
        ws.send_json(START)
        rec_id = ws.receive_json()["recording_id"]
        for f in tone_frames(12):
            ws.send_bytes(f)
        ws.send_json({"type": "stop"})
        collect_until(ws, "saved")
    return rec_id


def test_edits_persist(client, rec):
    r = client.patch(f"/api/recordings/{rec}", json={"title": "周会记录"})
    assert r.status_code == 200 and r.json()["title"] == "周会记录"
    detail = client.get(f"/api/recordings/{rec}").json()
    s0, s1 = detail["sentences"][0], detail["sentences"][1]

    r = client.patch(f"/api/recordings/{rec}/sentences/{s0['id']}", json={"text": "改过的第一句。"})
    assert r.json()["text"] == "改过的第一句。" and r.json()["edited"] is True
    # reassign to speaker 1 and to a brand-new speaker 3
    client.patch(f"/api/recordings/{rec}/sentences/{s1['id']}", json={"speaker_idx": 1})
    client.patch(f"/api/recordings/{rec}/sentences/{s0['id']}", json={"speaker_idx": 3})
    r = client.patch(f"/api/recordings/{rec}/speakers/1", json={"name": "张三"})
    assert {"idx": 1, "name": "张三"} in r.json()

    detail = client.get(f"/api/recordings/{rec}").json()
    assert detail["title"] == "周会记录"
    by_id = {s["id"]: s for s in detail["sentences"]}
    assert by_id[s0["id"]]["text"] == "改过的第一句。" and by_id[s0["id"]]["speaker_idx"] == 3
    assert by_id[s1["id"]]["speaker_idx"] == 1 and by_id[s1["id"]]["edited"] is False
    names = {s["idx"]: s["name"] for s in detail["speakers"]}
    assert names == {1: "张三", 2: "说话人2", 3: "说话人3"}

    txt = client.get(f"/api/recordings/{rec}/export.txt")
    assert txt.status_code == 200
    assert txt.headers["content-type"].startswith("text/plain")
    assert "attachment" in txt.headers["content-disposition"]
    body = txt.text
    lines = body.splitlines()
    assert lines[0] == "周会记录" and lines[2] == "时长：00:02"
    assert "[00:00] 说话人3：改过的第一句。" in lines
    assert "[00:01] 张三：第2句。" in lines
    assert "[00:02] 张三：第3句。" in lines


def test_validation_and_404(client, rec):
    assert client.get("/api/recordings/nope").status_code == 404
    assert client.patch(f"/api/recordings/{rec}", json={"title": ""}).status_code == 422
    assert client.patch(f"/api/recordings/{rec}/sentences/999999",
                        json={"text": "x"}).status_code == 404
    sid = client.get(f"/api/recordings/{rec}").json()["sentences"][0]["id"]
    assert client.patch(f"/api/recordings/{rec}/sentences/{sid}",
                        json={"speaker_idx": 0}).status_code == 422


def test_audio_range(client, rec):
    url = f"/api/recordings/{rec}/audio"
    full = client.get(url)
    assert full.status_code == 200 and full.headers["accept-ranges"] == "bytes"
    size = 44 + 12 * 3200 * 2
    assert len(full.content) == size and full.content[:4] == b"RIFF"
    assert full.headers["content-type"] == "audio/wav"

    part = client.get(url, headers={"Range": "bytes=0-43"})
    assert part.status_code == 206
    assert part.headers["content-range"] == f"bytes 0-43/{size}"
    assert part.content == full.content[:44]

    tail = client.get(url, headers={"Range": "bytes=-10"})
    assert tail.status_code == 206 and tail.content == full.content[-10:]

    open_end = client.get(url, headers={"Range": f"bytes={size - 100}-"})
    assert open_end.status_code == 206 and open_end.content == full.content[-100:]

    bad = client.get(url, headers={"Range": f"bytes={size}-"})
    assert bad.status_code == 416 and bad.headers["content-range"] == f"bytes */{size}"


def test_delete_removes_rows_and_wav(client, rec):
    path = Path(client.app.state.settings.audio_dir) / f"{rec}.wav"
    assert path.exists()
    assert client.delete(f"/api/recordings/{rec}").json() == {"ok": True}
    assert not path.exists()
    assert client.get("/api/recordings").json() == []
    assert client.get(f"/api/recordings/{rec}").status_code == 404
    assert client.delete(f"/api/recordings/{rec}").status_code == 404
    store = client.app.state.store
    assert store.list_sentences(rec) == [] and store.list_speakers(rec) == []


def test_cannot_delete_live_recording(client):
    token = pair(client)
    with client.websocket_connect("/device/ws",
                                  headers={"Authorization": f"Bearer {token}"}) as ws:
        ws.send_json(START)
        rec_id = ws.receive_json()["recording_id"]
        assert client.get("/api/recordings").json()[0]["live"] is True
        assert client.delete(f"/api/recordings/{rec_id}").status_code == 409
        ws.send_json({"type": "stop"})
        collect_until(ws, "saved")


def test_list_order_newest_first(client):
    token = pair(client)
    ids = []
    for _ in range(2):
        with client.websocket_connect("/device/ws",
                                      headers={"Authorization": f"Bearer {token}"}) as ws:
            ws.send_json(START)
            ids.append(ws.receive_json()["recording_id"])
            ws.send_json({"type": "stop"})
            collect_until(ws, "saved")
    listed = [r["id"] for r in client.get("/api/recordings").json()]
    assert set(listed) == set(ids)
    starts = [r["started_at"] for r in client.get("/api/recordings").json()]
    assert starts == sorted(starts, reverse=True)


def test_health_and_root(client):
    assert client.get("/api/health").json() == {"ok": True, "asr_configured": False}
    assert client.get("/").status_code == 200


def _speaker_count(client, rec_id):
    return next(r for r in client.get("/api/recordings").json()
                if r["id"] == rec_id)["speaker_count"]


def test_merge_speakers(client, rec):
    url = f"/api/recordings/{rec}/speakers/merge"
    detail = client.get(f"/api/recordings/{rec}").json()
    s0 = detail["sentences"][0]
    # create speaker 3 with one sentence, name speaker 1, then merge 2 and 3 into 1
    client.patch(f"/api/recordings/{rec}/sentences/{s0['id']}", json={"speaker_idx": 3})
    client.patch(f"/api/recordings/{rec}/speakers/1", json={"name": "张三"})
    assert _speaker_count(client, rec) == 3
    from2 = [s["id"] for s in client.get(f"/api/recordings/{rec}").json()["sentences"]
             if s["speaker_idx"] == 2]

    r = client.post(url, json={"from": 3, "into": 2})
    assert r.status_code == 200 and r.json()["moved"] == 1
    assert r.json()["speakers"] == [{"idx": 1, "name": "张三"}, {"idx": 2, "name": "说话人2"}]
    r = client.post(url, json={"from": 2, "into": 1})
    assert r.json()["moved"] == len(from2) + 1
    assert r.json()["speakers"] == [{"idx": 1, "name": "张三"}]

    detail = client.get(f"/api/recordings/{rec}").json()
    assert {s["speaker_idx"] for s in detail["sentences"]} == {1}
    assert detail["speakers"] == [{"idx": 1, "name": "张三"}]
    assert detail["speaker_count"] == 1 and _speaker_count(client, rec) == 1
    by_id = {s["id"]: s for s in detail["sentences"]}
    assert all(by_id[i]["edited"] is False for i in from2)  # same as per-sentence reassignment

    lines = client.get(f"/api/recordings/{rec}/export.txt").text.splitlines()
    assert "[00:00] 张三：第1句。" in lines
    assert not any("说话人" in line for line in lines[3:])


def test_merge_speakers_errors(client, rec):
    url = f"/api/recordings/{rec}/speakers/merge"
    assert client.post(url, json={"from": 2, "into": 2}).status_code == 400
    assert client.post(url, json={"from": 7, "into": 1}).status_code == 404
    assert client.post(url, json={"from": 1, "into": 7}).status_code == 404
    assert client.post("/api/recordings/nope/speakers/merge",
                       json={"from": 2, "into": 1}).status_code == 404
    assert client.post(url, json={"from": 0, "into": 1}).status_code == 422
    assert client.post(url, json={"into": 1}).status_code == 422
    # nothing changed
    assert [s["idx"] for s in client.get(f"/api/recordings/{rec}").json()["speakers"]] == [1, 2]


def test_cannot_merge_speakers_live(client):
    token = pair(client)
    with client.websocket_connect("/device/ws",
                                  headers={"Authorization": f"Bearer {token}"}) as ws:
        ws.send_json(START)
        rec_id = ws.receive_json()["recording_id"]
        for f in tone_frames(12):
            ws.send_bytes(f)
        collect_until(ws, "final")
        r = client.post(f"/api/recordings/{rec_id}/speakers/merge", json={"from": 2, "into": 1})
        assert r.status_code == 409
        ws.send_json({"type": "stop"})
        collect_until(ws, "saved")
    assert client.get(f"/api/recordings/{rec_id}").json()["speaker_count"] == 2


# ---- kind / notes -----------------------------------------------------------

SHANGHAI = ZoneInfo("Asia/Shanghai")


def local_ms(*args, tz=SHANGHAI) -> int:
    return int(datetime(*args, tzinfo=tz).timestamp() * 1000)


def add_rec(store, rec_id, started_at, texts, kind="note", duration_ms=1000):
    store.create_recording(rec_id, f"t-{rec_id}", "dev", "", kind=kind, started_at=started_at)
    for i, t in enumerate(texts):
        store.add_sentence(rec_id, 1, i * 1000, i * 1000 + 900, t)
    store.update_recording(rec_id, status="done", duration_ms=duration_ms)


def test_migration_adds_kind_to_old_database(tmp_path):
    from app.store import Store

    db = tmp_path / "old.db"
    con = sqlite3.connect(db)
    con.executescript("""
        CREATE TABLE recording (id TEXT PRIMARY KEY, title TEXT NOT NULL, device_id TEXT,
            started_at INTEGER NOT NULL, duration_ms INTEGER NOT NULL DEFAULT 0,
            status TEXT NOT NULL, audio_path TEXT, error TEXT);
        INSERT INTO recording(id, title, started_at, status) VALUES ('old1', '录音 10-01', 1, 'done');
    """)
    con.commit()
    con.close()
    store = Store(db)
    try:
        assert store.get_recording("old1")["kind"] == "meeting"
        assert [r["id"] for r in store.list_recordings("meeting")] == ["old1"]
        assert store.list_recordings("note") == []
        store.create_recording("new1", "笔记", "dev", "", kind="note")
        assert [r["id"] for r in store.list_recordings("note")] == ["new1"]
    finally:
        store.close()
    Store(db).close()  # re-opening an already migrated database is a no-op


def test_kind_filter(client):
    token = pair(client)
    ids = {}
    for mode in ("note", "meeting"):
        with client.websocket_connect("/device/ws",
                                      headers={"Authorization": f"Bearer {token}"}) as ws:
            ws.send_json({**START, "mode": mode})
            ids[mode] = ws.receive_json()["recording_id"]
            ws.send_json({"type": "stop"})
            collect_until(ws, "saved")
    all_ = client.get("/api/recordings").json()
    assert {r["id"]: r["kind"] for r in all_} == {ids["note"]: "note",
                                                  ids["meeting"]: "meeting"}
    assert [r["id"] for r in client.get("/api/recordings?kind=note").json()] == [ids["note"]]
    assert [r["id"] for r in client.get("/api/recordings?kind=meeting").json()] == [
        ids["meeting"]]
    assert client.get("/api/recordings?kind=bad").status_code == 422


def test_note_days_group_by_local_date(client):
    store = client.app.state.store
    # 23:59:30 on the 17th and 00:00:30 on the 18th in Asia/Shanghai are both the 17th in UTC
    add_rec(store, "a", local_ms(2026, 10, 17, 23, 59, 30), ["昨天最后一句。"], duration_ms=1500)
    add_rec(store, "b", local_ms(2026, 10, 18, 0, 0, 30), ["今天", "第一段。"], duration_ms=2000)
    add_rec(store, "c", local_ms(2026, 10, 18, 9, 5), ["第二段。"], duration_ms=3000)
    add_rec(store, "m", local_ms(2026, 10, 18, 10, 0), ["会议的话。"], kind="meeting")
    add_rec(store, "e", local_ms(2026, 10, 16, 8, 0), [])
    days = client.get("/api/notes/days").json()
    assert days == [
        {"date": "2026-10-18", "count": 2, "duration_ms": 5000, "first_text": "今天第一段。",
         "live": False},
        {"date": "2026-10-17", "count": 1, "duration_ms": 1500, "first_text": "昨天最后一句。",
         "live": False},
        {"date": "2026-10-16", "count": 1, "duration_ms": 1000, "first_text": "",
         "live": False},
    ]

    day = client.get("/api/notes/days/2026-10-18").json()
    assert day["date"] == "2026-10-18"
    segs = day["segments"]
    assert [s["id"] for s in segs] == ["b", "c"]
    assert [s["time"] for s in segs] == ["00:00", "09:05"]
    assert [x["text"] for x in segs[0]["sentences"]] == ["今天", "第一段。"]
    assert segs[0]["live"] is False and segs[0]["status"] == "done"
    assert [s["id"] for s in client.get("/api/notes/days/2026-10-17").json()["segments"]] == ["a"]
    assert client.get("/api/notes/days/2026-10-19").json()["segments"] == []
    assert client.get("/api/notes/days/2026-13-01").status_code == 422
    assert client.get("/api/notes/days/yesterday").status_code == 422


def test_note_days_follow_configured_timezone(make_client):
    client = make_client(timezone="UTC")
    store = client.app.state.store
    add_rec(store, "a", local_ms(2026, 10, 17, 23, 59, 30), ["x"])
    add_rec(store, "b", local_ms(2026, 10, 18, 0, 0, 30), ["y"])
    days = client.get("/api/notes/days").json()
    assert [(d["date"], d["count"]) for d in days] == [("2026-10-17", 2)]
    segs = client.get("/api/notes/days/2026-10-17").json()["segments"]
    assert [s["time"] for s in segs] == ["15:59", "16:00"]


def test_note_day_export_txt(client):
    store = client.app.state.store
    add_rec(store, "b", local_ms(2026, 10, 18, 8, 3), ["买牛奶，", "还有鸡蛋。"])
    add_rec(store, "e", local_ms(2026, 10, 18, 9, 0), [])  # empty segment is skipped
    add_rec(store, "c", local_ms(2026, 10, 18, 14, 30), ["下午三点开会。"])
    r = client.get("/api/notes/days/2026-10-18/export.txt")
    assert r.status_code == 200
    assert r.headers["content-type"].startswith("text/plain")
    assert "attachment" in r.headers["content-disposition"]
    assert r.text == "笔记 2026-10-18\n\n08:03 买牛奶，还有鸡蛋。\n14:30 下午三点开会。\n"


def test_note_day_live_segment(client):
    token = pair(client)
    with client.websocket_connect("/device/ws",
                                  headers={"Authorization": f"Bearer {token}"}) as ws:
        ws.send_json({**START, "mode": "note"})
        rec_id = ws.receive_json()["recording_id"]
        days = client.get("/api/notes/days").json()
        assert days[0]["live"] is True
        seg = client.get(f"/api/notes/days/{days[0]['date']}").json()["segments"][0]
        assert seg["id"] == rec_id and seg["live"] is True
        ws.send_json({"type": "stop"})
        collect_until(ws, "saved")
