# Implement plan — Web UI brand refresh

All paths under `server/web/`.

## Checklist (in order)

1. **Tokens & base** — rewrite `src/style.css`: tokens (light/dark, see design.md), base elements, buttons, inputs, select, cards, badges, `.page-head`, `.empty`, `.error/.warn/.ok`, focus ring, `.mono` utility, responsive helpers. Keep existing global class names (`card`, `row`, `spacer`, `muted`, `badge.*`, `button.primary/danger/link`, `.btn`) so views keep working mid-migration.
2. **Theme** — add `src/theme.js` (get/set preference, apply, media listener); inline anti-flash script + favicon + theme-color in `index.html`; add `public/favicon.svg`.
3. **Brand mark** — add `src/components/BrandMark.vue` (idle / rec / flat, reduced-motion).
4. **Shell** — rewrite `src/App.vue`: sidebar (desktop), top bar + drawer (mobile), nav icons, theme toggle.
5. **Speaker colors** — `src/util.js` `speakerColor` → `var(--spk-n)`.
6. **Pages** (one at a time, template + `<style>`, scripts untouched except presentational computed values): NoteDays → NoteDay → RecordingList → RecordingDetail → Hotwords → Pairing.
7. **Record console** — `src/views/Record.vue` layout + mark state mapping + narrow-screen notice. Do not modify recorder / WebSocket / state-machine code.
8. **Sweep** — grep for leftover colors: `rg -n "#[0-9a-fA-F]{3,8}\b|rgb\(|hsl\(" server/web/src` → only token definitions remain.

## Validation

```bash
cd server/web
npm install      # if node_modules missing
npm run build    # AC8
npm run dev      # manual checks; API proxied to :8080
```

Manual matrix (AC3–AC7): each page × {light, dark} × {1280px, 375px}; theme toggle persistence + reload without flash; OS theme change while on "system"; reduced-motion on; record start/stop/reconnect on desktop Chrome; transcript edit, speaker rename/merge, hotword add/move/delete, pairing confirm, export, delete.

## Risky files

- `src/views/Record.vue` (524 lines, live audio/WebSocket logic) — restructure template only; keep every `ref`, handler and `v-if` condition; diff the `<script>` block must be minimal (only added computed for mark state).
- `src/views/RecordingDetail.vue` — audio `timeupdate` → current sentence highlight; keep `ref="audio"` and class `current`.
- `index.html` inline script — must never throw (try/catch), or the app may not render.

## Rollback point

Before step 6, shell + tokens are self-contained; if page migration goes wrong, revert individual view files.

## Before `task.py start`

- implement.jsonl / check.jsonl curated (done).
- User approved the final planning summary.
