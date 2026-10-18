"""SQLite persistence (design.md §5)."""

from __future__ import annotations

import sqlite3
import threading
import time
from pathlib import Path
from typing import Any, Optional

SCHEMA = """
CREATE TABLE IF NOT EXISTS recording (
    id          TEXT PRIMARY KEY,
    title       TEXT NOT NULL,
    device_id   TEXT,
    started_at  INTEGER NOT NULL,          -- unix epoch ms
    duration_ms INTEGER NOT NULL DEFAULT 0,
    status      TEXT NOT NULL,             -- recording|done|asr_error|interrupted
    audio_path  TEXT,
    error       TEXT,
    kind        TEXT NOT NULL DEFAULT 'meeting'   -- meeting|note
);
CREATE TABLE IF NOT EXISTS speaker (
    recording_id TEXT NOT NULL REFERENCES recording(id) ON DELETE CASCADE,
    idx          INTEGER NOT NULL,          -- 1-based
    name         TEXT NOT NULL,
    PRIMARY KEY (recording_id, idx)
);
CREATE TABLE IF NOT EXISTS sentence (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    recording_id TEXT NOT NULL REFERENCES recording(id) ON DELETE CASCADE,
    idx          INTEGER NOT NULL,          -- 0-based order of arrival
    speaker_idx  INTEGER NOT NULL,
    start_ms     INTEGER NOT NULL,
    end_ms       INTEGER NOT NULL,
    text         TEXT NOT NULL,
    edited       INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS sentence_rec ON sentence(recording_id, idx);
CREATE TABLE IF NOT EXISTS device (
    id         TEXT PRIMARY KEY,
    token_hash TEXT NOT NULL,
    paired_at  INTEGER NOT NULL,
    name       TEXT
);
CREATE TABLE IF NOT EXISTS hotword (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    word       TEXT NOT NULL UNIQUE,
    position   INTEGER NOT NULL,
    created_at INTEGER NOT NULL
);
"""

# Run after migrations: may reference columns added by ALTER TABLE.
POST_MIGRATION = """
CREATE INDEX IF NOT EXISTS recording_kind ON recording(kind, started_at);
"""

KINDS = ("meeting", "note")

STATUSES = ("recording", "done", "asr_error", "interrupted")


def default_speaker_name(idx: int) -> str:
    return f"说话人{idx}"


def now_ms() -> int:
    return int(time.time() * 1000)


class Store:
    def __init__(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        self._lock = threading.RLock()
        self._db = sqlite3.connect(str(path), check_same_thread=False, isolation_level=None)
        self._db.row_factory = sqlite3.Row
        self._db.execute("PRAGMA foreign_keys=ON")
        self._db.execute("PRAGMA journal_mode=WAL")
        self._db.executescript(SCHEMA)
        self._migrate()
        self._db.executescript(POST_MIGRATION)

    def _migrate(self) -> None:
        cols = {r["name"] for r in self._db.execute("PRAGMA table_info(recording)").fetchall()}
        if "kind" not in cols:
            # Recordings made before the note mode existed are all meetings.
            self._db.execute(
                "ALTER TABLE recording ADD COLUMN kind TEXT NOT NULL DEFAULT 'meeting'"
            )

    def close(self) -> None:
        with self._lock:
            self._db.close()

    def _all(self, sql: str, args: tuple = ()) -> list[dict]:
        with self._lock:
            return [dict(r) for r in self._db.execute(sql, args).fetchall()]

    def _one(self, sql: str, args: tuple = ()) -> Optional[dict]:
        with self._lock:
            row = self._db.execute(sql, args).fetchone()
            return dict(row) if row else None

    def _exec(self, sql: str, args: tuple = ()) -> sqlite3.Cursor:
        with self._lock:
            return self._db.execute(sql, args)

    # ---- devices -------------------------------------------------------
    def upsert_device(self, device_id: str, token_hash: str, name: Optional[str]) -> None:
        self._exec(
            "INSERT INTO device(id, token_hash, paired_at, name) VALUES (?,?,?,?) "
            "ON CONFLICT(id) DO UPDATE SET token_hash=excluded.token_hash, "
            "paired_at=excluded.paired_at, name=COALESCE(excluded.name, device.name)",
            (device_id, token_hash, now_ms(), name),
        )

    def device_by_token_hash(self, token_hash: str) -> Optional[dict]:
        return self._one("SELECT * FROM device WHERE token_hash=?", (token_hash,))

    def list_devices(self) -> list[dict]:
        return self._all("SELECT id, name, paired_at FROM device ORDER BY paired_at DESC")

    def delete_device(self, device_id: str) -> bool:
        return self._exec("DELETE FROM device WHERE id=?", (device_id,)).rowcount > 0

    # ---- recordings ----------------------------------------------------
    def create_recording(
        self, rec_id: str, title: str, device_id: str, audio_path: str, kind: str = "meeting",
        started_at: Optional[int] = None,
    ) -> None:
        self._exec(
            "INSERT INTO recording(id, title, device_id, started_at, status, audio_path, kind) "
            "VALUES (?,?,?,?, 'recording', ?, ?)",
            (rec_id, title, device_id, now_ms() if started_at is None else started_at,
             audio_path, kind),
        )

    def update_recording(self, rec_id: str, **fields: Any) -> None:
        if not fields:
            return
        cols = ", ".join(f"{k}=?" for k in fields)
        self._exec(f"UPDATE recording SET {cols} WHERE id=?", (*fields.values(), rec_id))

    def get_recording(self, rec_id: str) -> Optional[dict]:
        return self._one("SELECT * FROM recording WHERE id=?", (rec_id,))

    def list_recordings(self, kind: Optional[str] = None) -> list[dict]:
        where, args = ("WHERE r.kind=?", (kind,)) if kind else ("", ())
        return self._all(
            f"""
            SELECT r.*,
              (SELECT COUNT(DISTINCT s.speaker_idx) FROM sentence s WHERE s.recording_id=r.id)
                AS speaker_count,
              (SELECT COUNT(*) FROM sentence s WHERE s.recording_id=r.id) AS sentence_count,
              (SELECT s.text FROM sentence s WHERE s.recording_id=r.id
                 ORDER BY s.start_ms, s.idx LIMIT 1) AS first_sentence
            FROM recording r {where} ORDER BY r.started_at DESC
            """,
            args,
        )

    def list_notes(self, start_ms: Optional[int] = None,
                   end_ms: Optional[int] = None) -> list[dict]:
        """Note segments (optionally with started_at in [start_ms, end_ms)), oldest first.

        ``text`` is the segment's sentences joined in order (None when it has none).
        """
        where, args = "", ()
        if start_ms is not None and end_ms is not None:
            where, args = "AND r.started_at>=? AND r.started_at<?", (start_ms, end_ms)
        return self._all(
            f"""
            SELECT r.*,
              (SELECT GROUP_CONCAT(t.text, '') FROM
                 (SELECT s.text FROM sentence s WHERE s.recording_id=r.id
                  ORDER BY s.start_ms, s.idx) t) AS text
            FROM recording r WHERE r.kind='note' {where} ORDER BY r.started_at, r.id
            """,
            args,
        )

    def recordings_with_status(self, status: str) -> list[dict]:
        return self._all("SELECT * FROM recording WHERE status=?", (status,))

    def delete_recording(self, rec_id: str) -> bool:
        with self._lock:
            self._db.execute("BEGIN")
            try:
                self._db.execute("DELETE FROM sentence WHERE recording_id=?", (rec_id,))
                self._db.execute("DELETE FROM speaker WHERE recording_id=?", (rec_id,))
                n = self._db.execute("DELETE FROM recording WHERE id=?", (rec_id,)).rowcount
                self._db.execute("COMMIT")
            except Exception:
                self._db.execute("ROLLBACK")
                raise
        return n > 0

    # ---- speakers ------------------------------------------------------
    def ensure_speaker(self, rec_id: str, idx: int) -> None:
        self._exec(
            "INSERT OR IGNORE INTO speaker(recording_id, idx, name) VALUES (?,?,?)",
            (rec_id, idx, default_speaker_name(idx)),
        )

    def list_speakers(self, rec_id: str) -> list[dict]:
        return self._all(
            "SELECT idx, name FROM speaker WHERE recording_id=? ORDER BY idx", (rec_id,)
        )

    def rename_speaker(self, rec_id: str, idx: int, name: str) -> None:
        with self._lock:
            self.ensure_speaker(rec_id, idx)
            self._exec(
                "UPDATE speaker SET name=? WHERE recording_id=? AND idx=?", (name, rec_id, idx)
            )

    def speaker_exists(self, rec_id: str, idx: int) -> bool:
        return bool(self._one(
            "SELECT 1 AS x FROM speaker WHERE recording_id=? AND idx=? "
            "UNION SELECT 1 FROM sentence WHERE recording_id=? AND speaker_idx=? LIMIT 1",
            (rec_id, idx, rec_id, idx),
        ))

    def merge_speakers(self, rec_id: str, src: int, dst: int) -> int:
        """Move every sentence of speaker `src` to `dst` and drop `src`, atomically.

        Like a per-sentence reassignment this does not set `edited`; `dst` keeps its name.
        Returns the number of sentences moved.
        """
        with self._lock:
            self._db.execute("BEGIN")
            try:
                self._db.execute(
                    "INSERT OR IGNORE INTO speaker(recording_id, idx, name) VALUES (?,?,?)",
                    (rec_id, dst, default_speaker_name(dst)),
                )
                n = self._db.execute(
                    "UPDATE sentence SET speaker_idx=? WHERE recording_id=? AND speaker_idx=?",
                    (dst, rec_id, src),
                ).rowcount
                self._db.execute(
                    "DELETE FROM speaker WHERE recording_id=? AND idx=?", (rec_id, src)
                )
                self._db.execute("COMMIT")
            except Exception:
                self._db.execute("ROLLBACK")
                raise
        return n

    # ---- sentences -----------------------------------------------------
    def add_sentence(
        self, rec_id: str, speaker_idx: int, start_ms: int, end_ms: int, text: str
    ) -> dict:
        with self._lock:
            self.ensure_speaker(rec_id, speaker_idx)
            row = self._db.execute(
                "SELECT COALESCE(MAX(idx), -1) + 1 FROM sentence WHERE recording_id=?", (rec_id,)
            ).fetchone()
            idx = row[0]
            cur = self._db.execute(
                "INSERT INTO sentence(recording_id, idx, speaker_idx, start_ms, end_ms, text) "
                "VALUES (?,?,?,?,?,?)",
                (rec_id, idx, speaker_idx, start_ms, end_ms, text),
            )
            return self.get_sentence(rec_id, cur.lastrowid)

    def get_sentence(self, rec_id: str, sid: int) -> Optional[dict]:
        return self._one(
            "SELECT id, idx, speaker_idx, start_ms, end_ms, text, edited FROM sentence "
            "WHERE recording_id=? AND id=?",
            (rec_id, sid),
        )

    def list_sentences(self, rec_id: str) -> list[dict]:
        return self._all(
            "SELECT id, idx, speaker_idx, start_ms, end_ms, text, edited FROM sentence "
            "WHERE recording_id=? ORDER BY start_ms, idx",
            (rec_id,),
        )

    def count_sentences(self, rec_id: str) -> int:
        row = self._one("SELECT COUNT(*) AS n FROM sentence WHERE recording_id=?", (rec_id,))
        return row["n"] if row else 0

    def update_sentence(
        self, rec_id: str, sid: int, text: Optional[str] = None, speaker_idx: Optional[int] = None
    ) -> None:
        with self._lock:
            if speaker_idx is not None:
                self.ensure_speaker(rec_id, speaker_idx)
                self._exec(
                    "UPDATE sentence SET speaker_idx=? WHERE recording_id=? AND id=?",
                    (speaker_idx, rec_id, sid),
                )
            if text is not None:
                self._exec(
                    "UPDATE sentence SET text=?, edited=1 WHERE recording_id=? AND id=?",
                    (text, rec_id, sid),
                )

    # ---- hotwords ------------------------------------------------------
    def list_hotwords(self) -> list[dict]:
        return self._all("SELECT id, word, position FROM hotword ORDER BY position, id")

    def get_hotword(self, hid: int) -> Optional[dict]:
        return self._one("SELECT id, word, position FROM hotword WHERE id=?", (hid,))

    def add_hotwords(self, words: list[str]) -> tuple[int, int]:
        """Append words at the end in the given order; already-present words are skipped.

        Returns (added, skipped).
        """
        added = skipped = 0
        with self._lock:
            self._db.execute("BEGIN")
            try:
                pos = self._db.execute(
                    "SELECT COALESCE(MAX(position), -1) + 1 FROM hotword").fetchone()[0]
                for w in words:
                    cur = self._db.execute(
                        "INSERT OR IGNORE INTO hotword(word, position, created_at) VALUES (?,?,?)",
                        (w, pos, now_ms()),
                    )
                    if cur.rowcount:
                        added += 1
                        pos += 1
                    else:
                        skipped += 1
                self._db.execute("COMMIT")
            except Exception:
                self._db.execute("ROLLBACK")
                raise
        return added, skipped

    def hotword_taken(self, word: str, exclude_id: int) -> bool:
        return bool(self._one(
            "SELECT 1 AS x FROM hotword WHERE word=? AND id<>?", (word, exclude_id)))

    def update_hotword(self, hid: int, word: str) -> None:
        self._exec("UPDATE hotword SET word=? WHERE id=?", (word, hid))

    def delete_hotword(self, hid: int) -> bool:
        return self._exec("DELETE FROM hotword WHERE id=?", (hid,)).rowcount > 0

    def reorder_hotwords(self, ids: list[int]) -> None:
        with self._lock:
            self._db.execute("BEGIN")
            try:
                for pos, hid in enumerate(ids):
                    self._db.execute("UPDATE hotword SET position=? WHERE id=?", (pos, hid))
                self._db.execute("COMMIT")
            except Exception:
                self._db.execute("ROLLBACK")
                raise
