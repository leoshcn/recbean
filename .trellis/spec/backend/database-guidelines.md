# Database Guidelines

Persistence is plain `sqlite3` (no ORM) behind one class, `Store`, in `server/app/store.py`.
Database file: `Settings.db_path` = `$DATA_DIR/recbean.db`; audio WAVs live in
`$DATA_DIR/audio/` and are referenced by `recording.audio_path`.

## Connection model

- One connection, `check_same_thread=False`, `isolation_level=None` (autocommit),
  `row_factory=sqlite3.Row`, `PRAGMA foreign_keys=ON`, `PRAGMA journal_mode=WAL`.
- Every access goes through `self._lock` (an `RLock`) — the store is shared by sync REST
  handlers (threadpool) and the async gateways. Use the helpers:
  - `_all(sql, args) -> list[dict]`, `_one(sql, args) -> dict | None`, `_exec(sql, args) -> Cursor`.
- Rows are returned as plain `dict`s, never `sqlite3.Row`, so API code can mutate/serialize them.

## Schema & migrations

- Schema is the `SCHEMA` string (`CREATE TABLE IF NOT EXISTS …`), executed on every start.
- Column additions go in `Store._migrate()`: inspect `PRAGMA table_info(...)`, then
  `ALTER TABLE … ADD COLUMN … DEFAULT …` with a comment on how old rows are interpreted
  (see the `kind` column: old recordings are meetings).
- Indexes that reference migrated columns go in `POST_MIGRATION`, which runs after `_migrate()`.
- Enumerations are TEXT columns validated in Python: `KINDS`, `STATUSES` constants in
  `store.py`; comment the allowed values next to the column.
- Timestamps are `INTEGER` unix epoch **milliseconds** (`now_ms()`); durations are `*_ms`.
- Speaker `idx` is 1-based; sentence `idx` is 0-based arrival order — keep these comments in
  sync with any change.

## Transactions

Multi-statement writes use an explicit transaction inside the lock:

```python
with self._lock:
    self._db.execute("BEGIN")
    try:
        ...
        self._db.execute("COMMIT")
    except Exception:
        self._db.execute("ROLLBACK")
        raise
```

Examples: `delete_recording`, `merge_speakers`, `add_hotwords`, `reorder_hotwords`.
Single statements rely on autocommit.

## Query rules

- Always parameterize values with `?`. f-strings are allowed only for fixed SQL fragments
  built from code (column names in `update_recording(**fields)`, a `WHERE` clause chosen
  from constants) — never for user input.
- `update_recording(**fields)` trusts its keyword names; only call it with literal keywords.
- Ordering is explicit in SQL (`ORDER BY start_ms, idx`, `ORDER BY position, id`), the API
  does not re-sort.
- Idempotent inserts use `INSERT OR IGNORE` / `ON CONFLICT … DO UPDATE` (`ensure_speaker`,
  `upsert_device`).

## Tests

`tests/test_api.py` exercises the store through the HTTP API on a temp `data_dir`; a
migration test opens a pre-migration DB with `sqlite3` directly. Add a similar test for any
new migration.
