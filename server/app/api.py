"""REST API (design.md §6) and device pairing endpoints (design.md §3)."""

from __future__ import annotations

import logging
import re
from datetime import date as Date, datetime, timedelta
from pathlib import Path
from typing import Optional
from urllib.parse import quote

from fastapi import APIRouter, HTTPException, Request
from fastapi.responses import JSONResponse, PlainTextResponse, Response, StreamingResponse

from .hotwords import estimate_tokens, normalize_word, select_active, validate_word
from .models import (HotwordOrder, HotwordPatch, HotwordsAdd, PairConfirm, PairRequest,
                     RecordingPatch, SentencePatch, SpeakerMerge, SpeakerPatch)
from .store import KINDS

log = logging.getLogger("recbean.api")

api = APIRouter(prefix="/api")
device_http = APIRouter(prefix="/device")


def _store(request: Request):
    return request.app.state.store


def _recording_or_404(request: Request, rec_id: str) -> dict:
    rec = _store(request).get_recording(rec_id)
    if not rec:
        raise HTTPException(404, "recording not found")
    return rec


def _public(rec: dict, request: Request) -> dict:
    out = {k: rec[k] for k in ("id", "title", "device_id", "started_at", "duration_ms",
                               "status", "error", "kind") if k in rec}
    for k in ("speaker_count", "sentence_count", "first_sentence"):
        if k in rec:
            out[k] = rec[k]
    out["live"] = rec["id"] in request.app.state.gateway.sessions
    return out


# ---- recordings ---------------------------------------------------------------

@api.get("/recordings")
def list_recordings(request: Request, kind: Optional[str] = None):
    if kind is not None and kind not in KINDS:
        raise HTTPException(422, f"kind must be one of {', '.join(KINDS)}")
    return [_public(r, request) for r in _store(request).list_recordings(kind)]


@api.get("/recordings/{rec_id}")
def get_recording(rec_id: str, request: Request):
    rec = _recording_or_404(request, rec_id)
    store = _store(request)
    out = _public(rec, request)
    sentences = store.list_sentences(rec_id)
    speakers = store.list_speakers(rec_id)
    out["speakers"] = speakers
    out["sentences"] = [{**s, "edited": bool(s["edited"])} for s in sentences]
    out["speaker_count"] = len({s["speaker_idx"] for s in sentences})
    out["sentence_count"] = len(sentences)
    return out


@api.patch("/recordings/{rec_id}")
def patch_recording(rec_id: str, body: RecordingPatch, request: Request):
    _recording_or_404(request, rec_id)
    _store(request).update_recording(rec_id, title=body.title.strip())
    return get_recording(rec_id, request)


@api.delete("/recordings/{rec_id}")
def delete_recording(rec_id: str, request: Request):
    rec = _recording_or_404(request, rec_id)
    if rec_id in request.app.state.gateway.sessions:
        raise HTTPException(409, "recording in progress")
    _store(request).delete_recording(rec_id)
    path = rec.get("audio_path")
    if path:
        try:
            Path(path).unlink(missing_ok=True)
        except OSError as e:
            log.error("failed to delete %s: %s", path, e)
    return {"ok": True}


@api.patch("/recordings/{rec_id}/sentences/{sid}")
def patch_sentence(rec_id: str, sid: int, body: SentencePatch, request: Request):
    _recording_or_404(request, rec_id)
    store = _store(request)
    if not store.get_sentence(rec_id, sid):
        raise HTTPException(404, "sentence not found")
    store.update_sentence(rec_id, sid, text=body.text, speaker_idx=body.speaker_idx)
    s = store.get_sentence(rec_id, sid)
    return {**s, "edited": bool(s["edited"])}


@api.patch("/recordings/{rec_id}/speakers/{idx}")
def patch_speaker(rec_id: str, idx: int, body: SpeakerPatch, request: Request):
    _recording_or_404(request, rec_id)
    if not 1 <= idx <= 99:
        raise HTTPException(422, "bad speaker idx")
    store = _store(request)
    store.rename_speaker(rec_id, idx, body.name.strip())
    return store.list_speakers(rec_id)


@api.post("/recordings/{rec_id}/speakers/merge")
def merge_speakers(rec_id: str, body: SpeakerMerge, request: Request):
    """Merge speaker `from` into `into` (D7): all its sentences move, `from` is removed."""
    _recording_or_404(request, rec_id)
    src, dst = body.from_idx, body.into
    if src == dst:
        raise HTTPException(400, "cannot merge a speaker into itself")
    if rec_id in request.app.state.gateway.sessions:
        raise HTTPException(409, "recording in progress")
    store = _store(request)
    for idx in (src, dst):
        if not store.speaker_exists(rec_id, idx):
            raise HTTPException(404, f"speaker {idx} not found")
    moved = store.merge_speakers(rec_id, src, dst)
    log.info("recording %s: merged speaker %d into %d (%d sentences)", rec_id, src, dst, moved)
    return {"moved": moved, "speakers": store.list_speakers(rec_id)}


_RANGE_RE = re.compile(r"^bytes=(\d*)-(\d*)$")


@api.get("/recordings/{rec_id}/audio")
def get_audio(rec_id: str, request: Request):
    rec = _recording_or_404(request, rec_id)
    path = Path(rec.get("audio_path") or "")
    if not path.is_file():
        raise HTTPException(404, "audio not found")
    size = path.stat().st_size
    headers = {"Accept-Ranges": "bytes", "Cache-Control": "no-cache"}
    start, end, status = 0, size - 1, 200
    rng = request.headers.get("range")
    if rng:
        m = _RANGE_RE.match(rng.strip().split(",")[0].strip())
        if not m or (not m.group(1) and not m.group(2)):
            return Response(status_code=416, headers={"Content-Range": f"bytes */{size}"})
        if m.group(1):
            start = int(m.group(1))
            end = min(int(m.group(2)), size - 1) if m.group(2) else size - 1
        else:  # suffix range: last N bytes
            start = max(size - int(m.group(2)), 0)
        if start >= size or start > end:
            return Response(status_code=416, headers={"Content-Range": f"bytes */{size}"})
        status = 206
        headers["Content-Range"] = f"bytes {start}-{end}/{size}"
    length = end - start + 1
    headers["Content-Length"] = str(length)

    def body():
        with open(path, "rb") as f:
            f.seek(start)
            remaining = length
            while remaining > 0:
                chunk = f.read(min(65536, remaining))
                if not chunk:
                    break
                remaining -= len(chunk)
                yield chunk

    return StreamingResponse(body(), status_code=status, media_type="audio/wav", headers=headers)


def fmt_ms(ms: int) -> str:
    s = max(int(ms), 0) // 1000
    h, rem = divmod(s, 3600)
    m, sec = divmod(rem, 60)
    return f"{h:02d}:{m:02d}:{sec:02d}" if h else f"{m:02d}:{sec:02d}"


def export_text(rec: dict, speakers: list[dict], sentences: list[dict], tz) -> str:
    names = {s["idx"]: s["name"] for s in speakers}
    started = datetime.fromtimestamp(rec["started_at"] / 1000, tz)
    lines = [
        rec["title"],
        f"录音时间：{started.strftime('%Y-%m-%d %H:%M:%S')}",
        f"时长：{fmt_ms(rec['duration_ms'])}",
        "",
    ]
    for s in sentences:
        name = names.get(s["speaker_idx"], f"说话人{s['speaker_idx']}")
        lines.append(f"[{fmt_ms(s['start_ms'])}] {name}：{s['text']}")
    return "\n".join(lines) + "\n"


@api.get("/recordings/{rec_id}/export.txt")
def export_txt(rec_id: str, request: Request):
    rec = _recording_or_404(request, rec_id)
    store = _store(request)
    text = export_text(rec, store.list_speakers(rec_id), store.list_sentences(rec_id),
                       request.app.state.gateway.tz)
    fname = re.sub(r'[\\/:*?"<>|]+', "_", rec["title"]) + ".txt"
    disp = f"attachment; filename=\"{rec_id}.txt\"; filename*=UTF-8''{quote(fname)}"
    return PlainTextResponse(text, media_type="text/plain; charset=utf-8",
                             headers={"Content-Disposition": disp})


# ---- notes (kind='note', grouped per local day) ---------------------------------

PREVIEW_CHARS = 80


def _tz(request: Request):
    return request.app.state.gateway.tz


def _local(ms: int, tz) -> datetime:
    return datetime.fromtimestamp(ms / 1000, tz)


def _day_range_ms(day: Date, tz) -> tuple[int, int]:
    """[start, end) epoch ms of a calendar day in ``tz`` (None = server local time)."""
    start = datetime(day.year, day.month, day.day, tzinfo=tz)
    nxt = day + timedelta(days=1)
    end = datetime(nxt.year, nxt.month, nxt.day, tzinfo=tz)
    return int(start.timestamp() * 1000), int(end.timestamp() * 1000)


def _parse_day(value: str) -> Date:
    try:
        if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", value):
            raise ValueError
        return Date.fromisoformat(value)
    except ValueError:
        raise HTTPException(422, "date must be YYYY-MM-DD") from None


def _day_segments(request: Request, day: Date) -> list[dict]:
    store = _store(request)
    tz = _tz(request)
    sessions = request.app.state.gateway.sessions
    start, end = _day_range_ms(day, tz)
    out = []
    for r in store.list_notes(start, end):
        sentences = store.list_sentences(r["id"])
        out.append({
            "id": r["id"], "title": r["title"], "started_at": r["started_at"],
            "time": _local(r["started_at"], tz).strftime("%H:%M"),
            "duration_ms": r["duration_ms"], "status": r["status"], "error": r["error"],
            "live": r["id"] in sessions,
            "sentences": [{**s, "edited": bool(s["edited"])} for s in sentences],
        })
    return out


@api.get("/notes/days")
def note_days(request: Request):
    """One entry per local day that has notes, newest day first."""
    tz = _tz(request)
    sessions = request.app.state.gateway.sessions
    days: dict[str, dict] = {}
    for r in _store(request).list_notes():  # oldest first
        key = _local(r["started_at"], tz).strftime("%Y-%m-%d")
        d = days.setdefault(key, {"date": key, "count": 0, "duration_ms": 0,
                                  "first_text": "", "live": False})
        d["count"] += 1
        d["duration_ms"] += r["duration_ms"] or 0
        d["live"] = d["live"] or r["id"] in sessions
        if not d["first_text"] and r["text"]:
            d["first_text"] = r["text"][:PREVIEW_CHARS]
    return sorted(days.values(), key=lambda d: d["date"], reverse=True)


@api.get("/notes/days/{day}")
def note_day(day: str, request: Request):
    d = _parse_day(day)
    return {"date": d.isoformat(), "segments": _day_segments(request, d)}


def export_note_day(day: Date, segments: list[dict]) -> str:
    lines = [f"笔记 {day.isoformat()}", ""]
    for seg in segments:
        text = "".join(s["text"] for s in seg["sentences"]).strip()
        if text:
            lines.append(f"{seg['time']} {text}")
    return "\n".join(lines) + "\n"


@api.get("/notes/days/{day}/export.txt")
def note_day_export(day: str, request: Request):
    d = _parse_day(day)
    text = export_note_day(d, _day_segments(request, d))
    fname = f"笔记 {d.isoformat()}.txt"
    disp = (f"attachment; filename=\"notes-{d.isoformat()}.txt\"; "
            f"filename*=UTF-8''{quote(fname)}")
    return PlainTextResponse(text, media_type="text/plain; charset=utf-8",
                             headers={"Content-Disposition": disp})


# ---- hotwords --------------------------------------------------------------------

def _hotword_state(request: Request) -> dict:
    budget = request.app.state.settings.hotword_token_budget
    rows = _store(request).list_hotwords()
    flags, used = select_active([r["word"] for r in rows], budget)
    return {
        "budget": budget,
        "used": used,
        "items": [{"id": r["id"], "word": r["word"], "tokens": estimate_tokens(r["word"]),
                   "active": ok} for r, ok in zip(rows, flags)],
    }


def _clean_word(raw: str) -> str:
    w = normalize_word(raw)
    err = validate_word(w)
    if err:
        raise HTTPException(422, f"{err}：{w[:40]}" if w else err)
    return w


@api.get("/hotwords")
def list_hotwords(request: Request):
    return _hotword_state(request)


@api.post("/hotwords")
def add_hotwords(body: HotwordsAdd, request: Request):
    """Append words (one per item; items may also contain several lines). Blank lines are
    ignored, duplicates (with each other or existing words) are skipped."""
    words = [normalize_word(line) for raw in body.words for line in str(raw).splitlines()]
    words = [w for w in words if w]
    if not words:
        raise HTTPException(422, "热词不能为空")
    words = [_clean_word(w) for w in words]
    added, skipped = _store(request).add_hotwords(words)
    log.info("hotwords: %d added, %d skipped", added, skipped)
    return {"added": added, "skipped": skipped, **_hotword_state(request)}


@api.patch("/hotwords/{hid}")
def patch_hotword(hid: int, body: HotwordPatch, request: Request):
    store = _store(request)
    if not store.get_hotword(hid):
        raise HTTPException(404, "hotword not found")
    word = _clean_word(body.word)
    if store.hotword_taken(word, hid):
        raise HTTPException(409, f"热词已存在：{word}")
    store.update_hotword(hid, word)
    return _hotword_state(request)


@api.delete("/hotwords/{hid}")
def delete_hotword(hid: int, request: Request):
    if not _store(request).delete_hotword(hid):
        raise HTTPException(404, "hotword not found")
    return _hotword_state(request)


@api.put("/hotwords/order")
def reorder_hotwords(body: HotwordOrder, request: Request):
    store = _store(request)
    current = sorted(r["id"] for r in store.list_hotwords())
    if sorted(body.ids) != current:
        raise HTTPException(400, "ids must be a permutation of all hotword ids")
    store.reorder_hotwords(body.ids)
    return _hotword_state(request)


# ---- pairing (web side) -------------------------------------------------------

@api.get("/pairings")
def list_pairings(request: Request):
    return [
        {"device_id": p.device_id, "code": p.code, "created_at": int(p.created_at * 1000)}
        for p in request.app.state.pairing.pending()
    ]


@api.post("/pairings")
def confirm_pairing(body: PairConfirm, request: Request):
    p = request.app.state.pairing.confirm(body.code, body.name)
    if not p:
        raise HTTPException(404, "配对码不存在或已过期")
    log.info("device %s paired", p.device_id)
    return {"ok": True, "device_id": p.device_id}


@api.get("/devices")
def list_devices(request: Request):
    return _store(request).list_devices()


@api.delete("/devices/{device_id}")
def delete_device(device_id: str, request: Request):
    if not _store(request).delete_device(device_id):
        raise HTTPException(404, "device not found")
    return {"ok": True}


# ---- pairing (device side) ----------------------------------------------------

@device_http.post("/pair")
def device_pair(body: PairRequest, request: Request):
    mgr = request.app.state.pairing
    p = mgr.request(body.device_id.strip())
    log.info("pairing requested by %s code=%s", p.device_id, p.code)
    return {"code": p.code, "expires_in": int(mgr.ttl_s)}


@device_http.get("/pair/{device_id}")
def device_pair_poll(device_id: str, request: Request):
    p = request.app.state.pairing.poll(device_id)
    if not p:
        return JSONResponse({"status": "unknown"}, status_code=404)
    if p.confirmed:
        return {"status": "paired", "token": p.token}
    return {"status": "pending", "code": p.code}


@api.get("/health")
def health(request: Request):
    return {"ok": True, "asr_configured": request.app.state.settings.doubao_configured}
