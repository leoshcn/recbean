import asyncio
import gzip
import json
import struct

import pytest

from app.asr import AsrError, FinalEvent, PartialEvent
from app.doubao_asr import (DoubaoAsrEngine, DoubaoConfig, ResultTracker, build_auth_headers,
                            build_request_params, decode_server_message, encode_audio_request,
                            encode_error_response, encode_full_client_request,
                            encode_server_response, extract_speaker)
from conftest import FIXTURES

RESP = json.loads((FIXTURES / "doubao_responses.json").read_text(encoding="utf-8"))


def parse_client_frame(buf: bytes) -> dict:
    flags = buf[1] & 0x0F
    out = {"header": buf[:4], "type": buf[1] >> 4, "flags": flags, "seq": None}
    rest = buf[4:]
    if flags & 0x01:
        out["seq"] = struct.unpack(">i", rest[:4])[0]
        rest = rest[4:]
    size = struct.unpack(">I", rest[:4])[0]
    out["payload"] = gzip.decompress(rest[4:4 + size])
    assert len(rest) == 4 + size
    return out


def test_full_client_request_frame():
    params = build_request_params("u1")
    f = parse_client_frame(encode_full_client_request(params, seq=1))
    assert f["header"] == bytes([0x11, 0x11, 0x11, 0x00])
    assert f["seq"] == 1
    body = json.loads(f["payload"])
    req = body["request"]
    assert req["model_name"] == "bigmodel"
    assert req["enable_speaker_info"] is True and req["ssd_version"] == "200"
    assert req["enable_nonstream"] is True and req["show_utterances"] is True
    assert req["enable_punc"] is True and req["enable_itn"] is True
    assert body["audio"] == {"format": "pcm", "codec": "raw", "rate": 16000, "bits": 16,
                             "channel": 1}
    assert "corpus" not in req  # no hotwords -> request unchanged


def test_request_params_note_mode_disables_diarization():
    req = build_request_params("u", speaker_info=False)["request"]
    assert req["enable_speaker_info"] is False
    assert "ssd_version" not in req
    assert req["enable_nonstream"] is True and "corpus" not in req


def test_request_params_hotwords_corpus_context():
    req = build_request_params("u", hotwords=["录音豆", "ESP32"])["request"]
    ctx = req["corpus"]["context"]
    assert isinstance(ctx, str) and "录音豆" in ctx  # ensure_ascii=False
    assert json.loads(ctx) == {"hotwords": [{"word": "录音豆"}, {"word": "ESP32"}]}
    assert build_request_params("u", hotwords=[])["request"].get("corpus") is None
    # survives the gzip/JSON frame encoding
    f = parse_client_frame(encode_full_client_request(
        build_request_params("u", speaker_info=False, hotwords=["豆包"]), seq=1))
    body = json.loads(f["payload"])
    assert json.loads(body["request"]["corpus"]["context"])["hotwords"] == [{"word": "豆包"}]


def test_engine_factory_kind_and_hotwords(tmp_path):
    from app.asr import UnconfiguredAsrEngine
    from app.doubao_asr import engine_factory_from_settings
    from conftest import make_settings

    settings = make_settings(tmp_path)
    factory = engine_factory_from_settings(settings)
    assert isinstance(factory("note", ["x"]), UnconfiguredAsrEngine)
    settings.doubao_api_key = "k"
    meeting = factory("meeting", [])
    note = factory("note", ["录音豆"])
    assert meeting.cfg.speaker_info is True and meeting.cfg.hotwords == []
    assert note.cfg.speaker_info is False and note.cfg.hotwords == ["录音豆"]


def test_audio_frames_sequence_and_last():
    pcm = bytes(range(256)) * 25
    f = parse_client_frame(encode_audio_request(pcm, seq=2, last=False))
    assert f["header"] == bytes([0x11, 0x21, 0x01, 0x00])
    assert f["seq"] == 2 and f["payload"] == pcm
    last = parse_client_frame(encode_audio_request(pcm, seq=7, last=True))
    assert last["header"] == bytes([0x11, 0x23, 0x01, 0x00])
    assert last["seq"] == -7
    noseq = encode_audio_request(b"", seq=None, last=True)
    assert noseq[:4] == bytes([0x11, 0x22, 0x01, 0x00])


def test_decode_hand_built_server_response():
    body = gzip.compress(json.dumps(RESP["doc_example"], ensure_ascii=False).encode())
    raw = bytes([0x11, 0x91, 0x11, 0x00]) + struct.pack(">iI", 1, len(body)) + body
    msg = decode_server_message(raw)
    assert msg.msg_type == 0b1001 and msg.sequence == 1 and not msg.is_last
    assert msg.payload["result"]["utterances"][1]["text"] == "今日头条母公司。"


def test_decode_last_response_negative_sequence():
    body = gzip.compress(b'{"result":{"text":""}}')
    raw = bytes([0x11, 0x93, 0x11, 0x00]) + struct.pack(">iI", -3, len(body)) + body
    msg = decode_server_message(raw)
    assert msg.is_last and msg.sequence == -3
    # our own encoder round-trips the same structure
    msg2 = decode_server_message(encode_server_response({"a": 1}, seq=-3, last=True))
    assert msg2.is_last and msg2.sequence == -3 and msg2.payload == {"a": 1}


def test_decode_error_frame():
    raw = encode_error_response(45000001, '{"error":"bad params"}')
    assert raw[:4] == bytes([0x11, 0xF0, 0x10, 0x00])
    msg = decode_server_message(raw)
    assert msg.is_error and msg.error_code == 45000001
    assert msg.payload == {"error": "bad params"}


def test_result_tracker_doc_example_all_final():
    events = ResultTracker().feed(RESP["doc_example"])
    assert [type(e) for e in events] == [FinalEvent, FinalEvent]
    assert events[0].text == "这是字节跳动，" and events[0].end_ms == 1705
    assert events[1].start_ms == 2110 and events[1].speaker is None


def test_result_tracker_stream():
    tracker = ResultTracker()
    for resp, expected in zip(RESP["stream"], RESP["stream_expected"]):
        got = []
        for e in tracker.feed(resp):
            if isinstance(e, PartialEvent):
                got.append({"type": "partial", "text": e.text})
            else:
                got.append({"type": "final", "text": e.text, "start_ms": e.start_ms,
                            "end_ms": e.end_ms, "speaker": e.speaker})
        assert got == expected
    # repeating the last full response must not duplicate finals
    assert tracker.feed(RESP["stream"][-1]) == []


@pytest.mark.parametrize("utt,expected", [
    ({"additions": {"speaker": "2"}}, "2"),
    ({"additions": {"speaker_id": 3}}, "3"),
    ({"additions": json.dumps({"speaker": "1"})}, "1"),
    ({"speaker": 0}, "0"),
    ({"additions": {"speaker": {"id": "5"}}}, "5"),
    ({"additions": {"log_id": "x"}}, None),
    ({}, None),
])
def test_extract_speaker(utt, expected):
    assert extract_speaker(utt) == expected


def test_real_capture_speaker_and_final():
    resp = RESP["real_definite_response"]
    assert extract_speaker(resp["result"]["utterances"][0]) == "0"
    finals = [e for e in ResultTracker().feed(resp) if isinstance(e, FinalEvent)]
    assert len(finals) == 1 and finals[0].speaker == "0" and finals[0].end_ms == 5912


def test_auth_headers():
    cfg = DoubaoConfig(url="wss://x", resource_id="volc.seedasr.sauc.duration",
                       app_key="app", access_key="acc")
    h = build_auth_headers(cfg, "cid")
    assert h["X-Api-App-Key"] == "app" and h["X-Api-Access-Key"] == "acc"
    assert h["X-Api-Resource-Id"] == "volc.seedasr.sauc.duration"
    assert h["X-Api-Connect-Id"] == "cid" and "X-Api-Key" not in h
    cfg2 = DoubaoConfig(url="wss://x", resource_id="r", api_key="k")
    assert build_auth_headers(cfg2, "c")["X-Api-Key"] == "k"


# ---- engine against a local fake Doubao server -----------------------------------

async def _run_engine_against_fake(mode: str):
    from websockets.asyncio.server import serve

    seen = {"headers": None, "frames": []}

    def process_response(conn, request, response):
        response.headers["X-Tt-Logid"] = "fake-logid-123"
        return response

    async def handler(ws):
        seen["headers"] = dict(ws.request.headers)
        first = parse_client_frame(await ws.recv())
        seen["frames"].append(first)
        if mode == "error":
            await ws.send(encode_error_response(45000001, "invalid request"))
            return
        i = 0
        async for raw in ws:
            f = parse_client_frame(raw)
            seen["frames"].append(f)
            last = f["flags"] == 0b0011
            if last:
                await ws.send(encode_server_response(RESP["stream"][-1], seq=-(i + 2), last=True))
                break
            if i < len(RESP["stream"]) - 1:
                await ws.send(encode_server_response(RESP["stream"][i], seq=i + 2))
            i += 1

    async with serve(handler, "127.0.0.1", 0, process_response=process_response) as server:
        port = server.sockets[0].getsockname()[1]
        engine = DoubaoAsrEngine(DoubaoConfig(url=f"ws://127.0.0.1:{port}", resource_id="res",
                                              app_key="a", access_key="b"))

        async def audio():
            for _ in range(4):
                yield bytes(6400)
                await asyncio.sleep(0.01)

        events = []

        async def emit(e):
            events.append(e)

        err = None
        try:
            await asyncio.wait_for(engine.run(audio(), emit), 10)
        except AsrError as e:
            err = e
        return engine, seen, events, err


def test_engine_end_to_end_with_fake_server():
    engine, seen, events, err = asyncio.run(_run_engine_against_fake("ok"))
    assert err is None
    assert engine.logid == "fake-logid-123"
    assert seen["headers"]["x-api-resource-id"] == "res"
    assert seen["headers"]["x-api-app-key"] == "a"
    seqs = [f["seq"] for f in seen["frames"]]
    assert seqs == [1, 2, 3, 4, -5]  # request, 3 audio, last audio negative
    assert seen["frames"][-1]["flags"] == 0b0011
    finals = [e for e in events if isinstance(e, FinalEvent)]
    assert [(f.text, f.speaker) for f in finals] == [("你好，请问是王先生吗？", "1"),
                                                     ("是的，我是。", "2")]


def test_engine_error_frame_raises():
    _, _, events, err = asyncio.run(_run_engine_against_fake("error"))
    assert isinstance(err, AsrError) and "45000001" in str(err)
    assert events == []


def test_engine_connect_failure_raises():
    engine = DoubaoAsrEngine(DoubaoConfig(url="ws://127.0.0.1:9", resource_id="r",
                                          app_key="a", access_key="b", open_timeout=2))

    async def audio():
        yield b""

    async def emit(e):
        pass

    with pytest.raises(AsrError):
        asyncio.run(engine.run(audio(), emit))


def test_gateway_with_doubao_engine_and_fake_server(make_client):
    """Device -> gateway -> DoubaoAsrEngine -> fake Doubao server -> events back to device."""
    import threading

    from websockets.asyncio.server import serve

    from app.doubao_asr import engine_factory_from_settings
    from conftest import collect_until, pair, tone_frames

    ready = threading.Event()
    state = {}

    async def handler(ws):
        parse_client_frame(await ws.recv())
        i = 0
        async for raw in ws:
            f = parse_client_frame(raw)
            if f["flags"] == 0b0011:
                await ws.send(encode_server_response(RESP["stream"][-1], seq=f["seq"], last=True))
                return
            if i < len(RESP["stream"]) - 1:
                await ws.send(encode_server_response(RESP["stream"][i], seq=f["seq"]))
            i += 1

    def server_thread():
        async def main():
            async with serve(handler, "127.0.0.1", 0) as server:
                state["port"] = server.sockets[0].getsockname()[1]
                state["stop"] = asyncio.Event()
                state["loop"] = asyncio.get_running_loop()
                ready.set()
                await state["stop"].wait()
        asyncio.run(main())

    t = threading.Thread(target=server_thread, daemon=True)
    t.start()
    assert ready.wait(5)
    try:
        holder = {}
        client = make_client(asr_factory=lambda *a: holder["factory"](*a))
        settings = client.app.state.settings
        settings.doubao_url = f"ws://127.0.0.1:{state['port']}"
        settings.doubao_app_key, settings.doubao_access_key = "a", "b"
        holder["factory"] = engine_factory_from_settings(settings)
        token = pair(client)
        with client.websocket_connect("/device/ws",
                                      headers={"Authorization": f"Bearer {token}"}) as ws:
            ws.send_json({"type": "start", "sample_rate": 16000, "ch": 1, "codec": "ima_adpcm"})
            rec_id = ws.receive_json()["recording_id"]
            for fr in tone_frames(6):
                ws.send_bytes(fr)
            ws.send_json({"type": "stop"})
            msgs = collect_until(ws, "saved")
        finals = [m for m in msgs if m["type"] == "final"]
        assert [(m["speaker"], m["text"]) for m in finals] == [
            (1, "你好，请问是王先生吗？"), (2, "是的，我是。")]
        assert {"type": "partial", "text": "你好"} in msgs
        assert msgs[-1]["sentences"] == 2
        rec = client.get(f"/api/recordings/{rec_id}").json()
        assert rec["status"] == "done" and rec["speaker_count"] == 2
    finally:
        state["loop"].call_soon_threadsafe(state["stop"].set)
        t.join(5)
