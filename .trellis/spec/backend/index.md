# Backend Development Guidelines

Conventions for the FastAPI server in `server/` (Python 3, SQLite, WebSockets, Doubao ASR).

## Guidelines Index

| Guide | Description |
|-------|-------------|
| [Directory Structure](./directory-structure.md) | Module responsibilities, app factory, where new code goes |
| [Database Guidelines](./database-guidelines.md) | `Store` (sqlite3, lock, WAL), schema & migrations, transactions, query rules |
| [Error Handling](./error-handling.md) | HTTP status matrix, WS error events, AsrError path, startup recovery |
| [Logging Guidelines](./logging-guidelines.md) | Logger names, format, levels, what never to log |
| [Quality Guidelines](./quality-guidelines.md) | pytest setup & rules, code style, forbidden patterns, review checklist |
| [Doubao ASR & Deployment](./doubao-asr-and-deploy.md) | Verified Doubao ASR contract, speaker field, file-ASR access, test-machine deploy |
| [Browser Recording & Ingress Gateways](./browser-recording.md) | Gateway hook pattern for new audio sources, resume rule, browser capture / HTTPS-proxy constraints |

## Pre-Development Checklist

- New endpoint / schema / setting → directory-structure + database + error-handling guides.
- New audio source or protocol change → browser-recording.md and `server/README.md`.
- ASR changes → doubao-asr-and-deploy.md; never call the real API from tests.

## Quality Check

- `.venv/Scripts/python -m pytest` green; review checklist in quality-guidelines.md.

**Language**: All documentation should be written in **English**.
