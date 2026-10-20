# Design — Web UI brand refresh

## Boundaries

Only `server/web/` changes: `index.html`, `src/style.css`, `src/App.vue`, `src/util.js` (speaker colors), `src/views/*.vue` (template + `<style>`), plus new `src/theme.js`, `src/components/BrandMark.vue`, `public/favicon.svg`. No changes to `api.js`, `recorder.js`, router table, script logic of views (beyond wiring presentational state), backend, or firmware.

## Design tokens (`src/style.css`)

Single source of truth; components use only `var(--…)`.

| Token | Light | Dark |
|---|---|---|
| `--bg` | `oklch(98.6% 0.004 155)` | Ink `oklch(22% 0.035 165)` |
| `--surface` | `oklch(100% 0 0)` | `oklch(26% 0.035 165)` |
| `--surface-2` (sidebar / hover) | `oklch(96.5% 0.012 150)` Mist | `oklch(19% 0.03 165)` |
| `--fg` | `oklch(20% 0.02 165)` | Mist `oklch(96.5% 0.012 150)` |
| `--muted` | `oklch(48% 0.015 165)` | `color-mix(in oklch, var(--mist) 64%, var(--ink))` |
| `--border` | `oklch(91% 0.008 160)` | `oklch(32% 0.03 165)` |
| `--accent` (links, primary btn, mark) | Deep `oklch(52% 0.15 152)` | Bean `oklch(78% 0.2 148)` |
| `--on-accent` (text on primary btn) | white | Ink |
| `--accent-soft` | `color-mix(accent 14%, transparent)` | same formula |
| `--signal` (REC only) | `oklch(64% 0.21 27)` | same |
| `--danger`, `--warn`, `--ok`, `--info` | derived, verified ≥ 4.5:1 on `--surface` in both themes |
| `--spk-1..8` | 8 speaker hues, L≈50% light / L≈75% dark |

Also: `--font-display/body/mono` (from VI), `--radius: 10px`, `--radius-lg: 16px`, spacing `--gap-xs..xl` (8/12/20/32/56), `--shadow` (subtle, dark mode uses border instead of shadow).

Theme selectors: tokens on `:root`; dark overrides under `@media (prefers-color-scheme: dark) { :root:not([data-theme="light"]) {…} }` and `:root[data-theme="dark"] {…}`. Set `color-scheme: light dark` accordingly so native controls (`<audio>`, `<select>`, scrollbars) follow.

"Tech" feel comes from: monospace tabular numerals for all times/durations/codes/counters, thin 1px borders over heavy shadows, accent-soft glows on focus/active, the animated mark on the record console. No gradients/shadows on the logo (VI rule).

## Theme switching (`src/theme.js`)

- Preference: `'system' | 'light' | 'dark'`, key `recbean-theme`, all storage access in try/catch → fallback `'system'`.
- `system` → remove `data-theme`; otherwise set `document.documentElement.dataset.theme`.
- Anti-flash: a tiny inline `<script>` in `index.html` `<head>` applies the stored value before CSS paints (duplicated minimal logic, also try/catch).
- `theme-color` meta: two tags with `media="(prefers-color-scheme: …)"`; when forced, JS updates a single tag's content.
- Sidebar control: 3-segment toggle (icons: monitor / sun / moon, inline SVG, `aria-pressed`, labels for screen readers).

## Brand mark (`src/components/BrandMark.vue`)

- Copy `rb-mark` / `rb-mark-sm` geometry from `branding/recbean-brand-identity.html` (ellipse rx40 ry26 rotated −24°, sine slit width 7 / 10.5). Fill = `currentColor` → `--accent` in each theme (Deep on light, Bean on dark — matches VI rule).
- Props: `size`, `state: 'idle' | 'rec' | 'flat'`. `rec` animates the slit's stroke-dash/translate leftwards with a 1.6s linear infinite loop; `flat` morphs the slit path to a straight line (CSS `d` transition, fallback: swap path). `@media (prefers-reduced-motion: reduce)` disables animation.
- Sizes ≤ 24px use `-sm` (thick slit).
- `public/favicon.svg` = `rb-icon-sm` (Bean background, Ink mark), linked from `index.html`.

## Layout (`App.vue`)

- Desktop (≥ 768px): CSS grid `220px | 1fr`; sticky sidebar on `--surface-2` with mark + wordmark (`录音豆`, weight 600, letter-spacing +6%), nav list (icon + label, active = accent-soft pill + accent text), theme toggle at bottom. Main content max-width ~960px, padding 32px.
- Mobile (< 768px): sidebar becomes a 52px top bar (mark + wordmark + menu button); menu opens a drawer overlay (focus trap-lite: Esc closes, closes on route change, backdrop click).
- Active rule for 会议 keeps current logic (`/meetings` or `/r/`).
- Page header pattern (shared class `.page-head`): `h1` + meta + actions on the right; wraps on mobile.

## Pages

| Page | Changes |
|---|---|
| NoteDays | date list as cards/rows with mono date, weekday, segment count/duration; hover accent border |
| NoteDay | header with stats + actions; segments as timeline (mono time column + content); edit buttons always visible on touch (keep existing `hover:none` rule via token) |
| RecordingList | cards with title, status badge, mono meta row, first-sentence preview clamp 2 lines |
| RecordingDetail | header card (title edit, meta, actions) + full-width audio player; speaker chips; transcript with mono timestamps, current-sentence highlight via accent-soft + left accent bar |
| Hotwords | quota meter bar (accent, warn when over), add form, ordered list with mono index |
| Pairing | pending codes in large mono digits, device list |
| Record | console (below) |

Badges: soft-tinted pill per status (`recording` uses `--signal`, `done` `--ok`, `asr_error` `--warn`, `interrupted` `--info`, `source` accent-soft, `off` muted).

## Record console (`Record.vue`)

Layout (desktop only): left console panel — large `BrandMark` with state mapping (see PRD R6), big round start/stop button (start = accent, stop = signal-outlined), REC red dot + mono duration (`tabular-nums`, ~40px), connection badge; right/secondary — source options (mic checkbox + select, system audio checkbox), two level meters (accent fill, segmented "tech" look via repeating-linear-gradient on track), warnings. Below: live transcript panel with mono timestamps and speaker colors.

State → mark: computed in script from existing `state` ref only (presentational). No state machine changes.

Narrow screen (< 768px): render a notice card "请在桌面浏览器使用录音" instead of the console (`v-if` on a `matchMedia` ref or CSS-only hide). Prefer CSS-only to avoid touching mic acquisition logic; ensure hidden controls cannot be triggered.

## Speaker colors (`util.js`)

`speakerColor(idx)` returns `` `var(--spk-${n})` `` (n = 1..8). Callers use it in inline `style` (`color` / `background`), which accepts `var()` — call sites unchanged.

## Compatibility

- oklch + `color-mix` need Chrome 111+/Safari 16.4+/Firefox 113+. Acceptable for this self-hosted admin UI (recording already requires modern Chrome/Edge). No fallback.
- No new npm deps.

## Rollback

Pure frontend; revert the `server/web` changes and rebuild.
