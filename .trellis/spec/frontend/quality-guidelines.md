# Frontend Quality Guidelines

## Checks

- `cd server/web && npm run build` must pass (Windows: `npm.cmd run build`). There is no
  linter, formatter, type checker or unit-test setup in `server/web`; the build plus manual
  browser checks are the gate.
- Manual check matrix for UI changes: affected pages × light/dark × 1280px/375px; for
  `Record.vue` a real start → stop → reconnect cycle in desktop Chrome/Edge over HTTPS.
- Color sweep: `grep -rnE "#[0-9a-fA-F]{3,8}\b|rgb\(|hsl\(" server/web/src` → no hits.

## JavaScript conventions (no TypeScript)

- ES modules, `<script setup>`, 2-space indent, no semicolons, single quotes (match files).
- Data shapes are the server's JSON (snake_case fields such as `started_at`, `duration_ms`,
  `speaker_idx`); don't rename them client-side. Server-side validation lives in pydantic
  models (`server/app/models.py`); client checks are only for fast feedback.
- Times from the server are epoch ms; format only through `util.js`.
- Constants shared with the server are named and commented with their source
  (`BROWSER_DEVICE` ↔ `web_ws.py`, `MAX_CHARS` ↔ `hotwords.WORD_MAX_CHARS`).

## Accessibility

- Text contrast ≥ 4.5:1 in both themes; visible focus ring (`:focus-visible`).
- Keyboard: everything clickable is reachable — use buttons/links, or `tabindex="0"` +
  Enter handler on clickable cards. Don't hide focusable controls with `visibility:hidden`.
- Icon-only buttons need `aria-label`; toggles use `aria-pressed`.
- Respect `prefers-reduced-motion`.
- Touch: ≥ 40px targets under 768px; hover-only affordances must be visible under
  `@media (hover: none)`.

## Forbidden

- New npm dependencies without a strong reason (current deps: `vue`, `vue-router`, Vite).
- `fetch` outside `api.js`; hard-coded colors outside `style.css` tokens.
- Changing recorder/WebSocket logic as part of a visual change.
- Committing `server/web/dist/`.
