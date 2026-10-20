# Frontend Directory Structure

The Web UI is `server/web/`: Vue 3 (`<script setup>`, plain JavaScript) + vue-router 4 +
Vite 6. No UI library, no CSS framework, no state library. Built output `dist/` is
git-ignored and produced in Docker (`server/Dockerfile`) — never commit it.

## Layout

```
server/web/
├── index.html            # anti-flash theme script, theme-color metas, favicon
├── vite.config.js        # dev proxy: /api, /device (ws), /web/ws (ws) → 127.0.0.1:8080
├── public/               # served as-is at site root: favicon.svg, pcm-worklet.js (AudioWorklet)
└── src/
    ├── main.js           # router table (hash history) + app mount + style.css import
    ├── App.vue           # shell: sidebar / mobile drawer, theme toggle, <router-view>
    ├── style.css         # design tokens + shared component classes (see theming-and-brand.md)
    ├── api.js            # the only place that calls fetch(); `api.*` methods + URL builders
    ├── util.js           # formatting (fmtDuration/fmtTime/fmtDay), STATUS_TEXT, speakerColor
    ├── theme.js          # theme preference get/set/apply
    ├── recorder.js       # browser capture: mic/system audio, AudioWorklet, PCM framing
    ├── components/       # reusable presentational components (BrandMark.vue)
    └── views/            # one file per route
```

## Routes (`src/main.js`)

| Path | View | Notes |
|---|---|---|
| `/notes` | `NoteDays.vue` | default (`/` redirects here) |
| `/notes/:date` | `NoteDay.vue` | `props: true` |
| `/meetings` | `RecordingList.vue` | |
| `/r/:id` | `RecordingDetail.vue` | used for meetings and note segments; nav highlights 会议 |
| `/record` | `Record.vue` | desktop only |
| `/hotwords`, `/pairing` | `Hotwords.vue`, `Pairing.vue` | |

Hash history is required: the server mounts `dist/` with `StaticFiles(html=True)` and has
no SPA fallback route.

## Where new code goes

- New page → `views/X.vue` + route in `main.js` + nav item in `App.vue`.
- New endpoint call → method on `api` in `api.js` (encode path params with `enc`).
- Formatting/label helpers shared by ≥2 views → `util.js`.
- Reusable UI piece → `components/`; page-specific markup stays in the view.
- Browser audio/WebSocket plumbing → `recorder.js` / `Record.vue`, governed by
  `../backend/browser-recording.md`.
