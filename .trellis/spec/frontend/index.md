# Frontend Development Guidelines

Conventions for the Web UI in `server/web/` (Vue 3 `<script setup>`, plain JavaScript,
vue-router, Vite; no UI/state/CSS libraries).

## Guidelines Index

| Guide | Description |
|-------|-------------|
| [Directory Structure](./directory-structure.md) | File layout, routes, where new code goes |
| [Component Guidelines](./component-guidelines.md) | View structure, inline editing, destructive actions, template conventions |
| [State Management](./state-management.md) | Local refs, load pattern, polling for live data, localStorage rules |
| [Data Fetching & Error Display](./data-fetching.md) | `api.js` contract, error mapping, WebSocket ownership |
| [Theming, Brand & Layout](./theming-and-brand.md) | Design tokens, light/dark theme, BrandMark, app shell, responsive & VI rules |
| [Quality Guidelines](./quality-guidelines.md) | Build gate, manual check matrix, JS conventions, accessibility, forbidden patterns |

## Pre-Development Checklist

- Read the guides for the area you touch; any visual change → `theming-and-brand.md`.
- Changing an API call → check `server/app/api.py` and `../backend/error-handling.md`.
- Touching `Record.vue` / `recorder.js` → `../backend/browser-recording.md`.

## Quality Check

- `npm run build` passes; color sweep clean; manual matrix done (see quality guidelines).

**Language**: All documentation should be written in **English**.
