"""REST API (design.md §6) and device pairing endpoints (design.md §3)."""

from __future__ import annotations

import logging
import re
from datetime import datetime
from pathlib import Path
from urllib.parse import quote

from fastapi import APIRouter, HTTPException, Request
from fastapi.responses import JSONResponse, PlainTextResponse, Response, StreamingResponse

from .models import (PairConfirm, PairRequest, RecordingPatch, SentencePatch, SpeakerMerge,
                     SpeakerPatch)

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
                               "status", "error") if k in rec}
    for k in ("speaker_count", "sentence_count", "first_sentence"):
        if k in rec:
            out[k] = rec[k]
    out["live"] = rec["id"] in request.app.state.gateway.sessions
    return out


# ---- recordings ---------------------------------------------------------------

@api.get("/recordings")
def list_recordings(request: Request):
    return [_public(r, request) for r in _store(request).list_recordings()]


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
