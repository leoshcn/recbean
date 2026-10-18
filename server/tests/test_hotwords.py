import json

import pytest

from app.config import Settings
from app.hotwords import (active_words, corpus_context, estimate_tokens, normalize_word,
                          select_active, validate_word)


# ---- estimate / select ---------------------------------------------------------

@pytest.mark.parametrize("word,tokens", [
    ("录音豆", 3 + 1),
    ("ESP32", 1 + 1),            # one ASCII letter/digit run
    ("ESP32 开发板", 1 + 3 + 1),  # spaces are free
    ("Wi-Fi", 3 + 1),            # run, symbol, run
    ("GPT4o", 1 + 1),
    ("豆包ASR", 2 + 1 + 1),
])
def test_estimate_tokens(word, tokens):
    assert estimate_tokens(word) == tokens


def test_select_active_stops_at_first_word_over_budget():
    words = ["一二三", "四五六七八", "九"]  # 4, 6, 2 tokens
    assert select_active(words, 100) == ([True, True, True], 12)
    assert select_active(words, 12) == ([True, True, True], 12)
    assert select_active(words, 10) == ([True, True, False], 10)
    flags, used = select_active(words, 9)
    # "九" would still fit but comes after a word that did not: list order is priority
    assert flags == [True, False, False] and used == 4
    assert select_active(words, 3) == ([False, False, False], 0)
    assert select_active([], 100) == ([], 0)
    assert active_words(words, 10) == ["一二三", "四五六七八"]


def test_normalize_and_validate():
    assert normalize_word("  录音豆 \t") == "录音豆"
    assert validate_word("") is not None
    assert validate_word("字" * 32) is None
    assert validate_word("字" * 33) is not None


def test_corpus_context_is_json_string_without_ascii_escapes():
    ctx = corpus_context(["录音豆", "ESP32"])
    assert ctx == '{"hotwords": [{"word": "录音豆"}, {"word": "ESP32"}]}'
    assert json.loads(ctx) == {"hotwords": [{"word": "录音豆"}, {"word": "ESP32"}]}


def test_budget_from_env(monkeypatch):
    monkeypatch.delenv("HOTWORD_TOKEN_BUDGET", raising=False)
    assert Settings().hotword_token_budget == 100
    monkeypatch.setenv("HOTWORD_TOKEN_BUDGET", "40")
    assert Settings().hotword_token_budget == 40


# ---- API -------------------------------------------------------------------------

def words(state):
    return [i["word"] for i in state["items"]]


def test_crud_and_state(client):
    st = client.get("/api/hotwords").json()
    assert st == {"budget": 100, "used": 0, "items": []}

    r = client.post("/api/hotwords", json={"words": [" 录音豆 ", "豆包\n\n火山引擎\r\nESP32", "豆包"]})
    assert r.status_code == 200
    st = r.json()
    assert st["added"] == 4 and st["skipped"] == 1
    assert words(st) == ["录音豆", "豆包", "火山引擎", "ESP32"]
    assert [i["tokens"] for i in st["items"]] == [4, 3, 5, 2]
    assert st["used"] == 14 and all(i["active"] for i in st["items"])

    # existing word skipped
    st = client.post("/api/hotwords", json={"words": ["录音豆", "新词"]}).json()
    assert (st["added"], st["skipped"]) == (1, 1) and words(st)[-1] == "新词"

    ids = {i["word"]: i["id"] for i in st["items"]}
    st = client.patch(f"/api/hotwords/{ids['新词']}", json={"word": " 新名字 "}).json()
    assert words(st)[-1] == "新名字"
    assert client.patch(f"/api/hotwords/{ids['新词']}", json={"word": "豆包"}).status_code == 409
    # renaming to itself is fine
    assert client.patch(f"/api/hotwords/{ids['豆包']}", json={"word": "豆包"}).status_code == 200
    assert client.patch("/api/hotwords/9999", json={"word": "x"}).status_code == 404

    st = client.delete(f"/api/hotwords/{ids['火山引擎']}").json()
    assert "火山引擎" not in words(st) and st["used"] == 4 + 3 + 2 + 4
    assert client.delete(f"/api/hotwords/{ids['火山引擎']}").status_code == 404


@pytest.mark.parametrize("body", [
    {"words": []},
    {"words": ["", "  ", "\n"]},
    {"words": ["字" * 33]},
    {"words": ["好", "字" * 33]},
])
def test_add_rejects_empty_and_long(client, body):
    assert client.post("/api/hotwords", json=body).status_code == 422
    assert client.get("/api/hotwords").json()["items"] == []


@pytest.mark.parametrize("word", ["", "   ", "字" * 33, "a\nb"])
def test_patch_rejects_invalid(client, word):
    hid = client.post("/api/hotwords", json={"words": ["原词"]}).json()["items"][0]["id"]
    assert client.patch(f"/api/hotwords/{hid}", json={"word": word}).status_code == 422
    assert words(client.get("/api/hotwords").json()) == ["原词"]


def test_reorder(client):
    st = client.post("/api/hotwords", json={"words": ["甲", "乙", "丙"]}).json()
    a, b, c = (i["id"] for i in st["items"])
    st = client.put("/api/hotwords/order", json={"ids": [c, a, b]}).json()
    assert words(st) == ["丙", "甲", "乙"]
    assert words(client.get("/api/hotwords").json()) == ["丙", "甲", "乙"]
    for bad in ([a, b], [a, b, c, 999], [a, a, b], [a, b, 999]):
        assert client.put("/api/hotwords/order", json={"ids": bad}).status_code == 400
    # new words are appended after the reordered list
    st = client.post("/api/hotwords", json={"words": ["丁"]}).json()
    assert words(st) == ["丙", "甲", "乙", "丁"]


def test_over_budget_marked_inactive(make_client):
    client = make_client(hotword_token_budget=10)
    st = client.post("/api/hotwords", json={"words": ["一二三", "四五六七八", "九"]}).json()
    assert st["budget"] == 10 and st["used"] == 10
    assert [i["active"] for i in st["items"]] == [True, True, False]


def test_only_active_hotwords_reach_asr(make_client):
    from conftest import FakeAsr, collect_until, pair

    client = make_client(hotword_token_budget=10)
    client.post("/api/hotwords", json={"words": ["一二三", "四五六七八", "九"]})
    token = pair(client)
    with client.websocket_connect("/device/ws",
                                  headers={"Authorization": f"Bearer {token}"}) as ws:
        ws.send_json({"type": "start", "sample_rate": 16000, "ch": 1, "codec": "ima_adpcm"})
        ws.receive_json()
        ws.send_json({"type": "stop"})
        collect_until(ws, "saved")
    assert FakeAsr.calls[-1] == ("meeting", ["一二三", "四五六七八"])
