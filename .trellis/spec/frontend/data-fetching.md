# Data Fetching & Error Display

## `src/api.js` is the only HTTP layer

- `request(method, url, body)` sends JSON, returns `res.json()`, and on non-2xx throws
  `Error(detail)` where `detail` is the server's `{"detail": ...}` (string, or JSON of the
  pydantic error list), falling back to the status code string (`"404"`).
- Every endpoint is a method on `api`; path params go through `enc` (`encodeURIComponent`).
  Download/stream endpoints are URL builders, not requests: `audioUrl`, `exportUrl`,
  `noteDayExportUrl` (used in `<audio :src>` / `<a :href>`).
- Views never call `fetch` directly. When adding a REST endpoint on the server
  (`server/app/api.py`), add the matching `api.*` method in the same change.

## Error display

| Situation | Pattern | Example |
|---|---|---|
| Page load failed | `error` ref rendered as `<p class="error">` / alert block at top | `加载失败：${e.message}` |
| Action failed | `alert(\`保存失败：${e.message}\`)` | save, delete, merge, reorder |
| Known server conflicts | map to Chinese text | 409 / `in progress` → `录音仍在进行中`; 404 / `not found` → `录音不存在` |
| Client-side validation | check before calling the API and `alert` / inline message | hotword length (`MAX_CHARS` mirrors `hotwords.WORD_MAX_CHARS`) |

Server `detail` strings are English for API errors, Chinese for domain validation — when
mapping, match on both the status string and a substring, as `RecordingDetail.vue` does.

## WebSockets

Only `Record.vue` / `recorder.js` open sockets (`/web/ws`, protocol in `server/README.md`).
Reconnect/backoff/resume rules: `../backend/browser-recording.md`.
