# Theming, Brand & Layout (server/web)

Established in task `10-20-web-ui-brand-refresh` (deployed and accepted 2026-10-20).
Source of truth for visuals: `branding/recbean-brand-identity.html` (VI v1.0).

## Scope / Trigger

Read this before touching any `.vue` template/`<style>`, `src/style.css`, `src/theme.js`,
`src/components/BrandMark.vue`, `src/App.vue` or `index.html` in `server/web`.

## Files & signatures

| File | Contract |
|---|---|
| `src/style.css` | **Only** place colors are defined. Tokens on `:root`; dark overrides duplicated under `@media (prefers-color-scheme: dark) { :root:not([data-theme="light"]) }` **and** `:root[data-theme="dark"]`. Also hosts shared component classes. |
| `src/theme.js` | `THEMES = ['system','light','dark']`; `getTheme()`, `setTheme(mode) -> mode`, `applyTheme(mode)`. localStorage key `recbean-theme`; every storage access in try/catch, fallback `'system'`. `'system'` = no `data-theme` attribute (pure CSS follows OS; no matchMedia listener needed). |
| `index.html` | Inline `<head>` script applies stored theme before paint (anti-flash) — must stay in sync with `theme.js` key/values and never throw. Two `theme-color` metas with `media` attrs; `theme.js` rewrites them when forced. Favicon `public/favicon.svg` (= VI `rb-icon-sm`). |
| `src/components/BrandMark.vue` | Props `size` (px height, default 24), `state: 'idle' \| 'rec' \| 'flat'`, `label` (sets `role=img`, else `aria-hidden`). Fill = `currentColor`. ≤24px auto-uses the thick slit (10.5). `rec` = slit flows left, 1.6 s/period; `flat` = slit straightened. Animations off under `prefers-reduced-motion`. |
| `src/util.js` | `speakerColor(idx)` returns `` `var(--spk-${n})` `` (n = 1..8, idx starts at 1). Use it in inline `style` only. |
| `src/App.vue` | Shell: ≥768px sticky 228px sidebar (mark + wordmark, 5 nav items, 3-state theme toggle with `aria-pressed`); <768px 52px top bar + drawer (closes on Esc, backdrop, route change). "会议" is active for `/meetings` and `/r/*`. |

## Token contract

| Token | Use | Light | Dark |
|---|---|---|---|
| `--bg` / `--surface` / `--surface-2` | page / cards / sidebar & hover | near-white / white / Mist | Ink family |
| `--fg` / `--muted` | text / secondary text | Ink-ish | Mist-ish |
| `--border`, `--border-strong` | 1px hairlines (preferred over shadows) | | |
| `--accent` | links, primary button, logo | Deep `oklch(52% .15 152)` | Bean `oklch(78% .2 148)` |
| `--on-accent` | text on primary button | white | Ink |
| `--accent-soft` / `--accent-strong` | tinted bg / **text placed on a `-soft` tint** | | |
| `--danger --warn --ok --info` + `-soft` | status text / tinted badges | | |
| `--signal` (+`-soft`) | **REC state only** (red dot, recording badge, stop button) | `oklch(64% .21 27)` | same |
| `--spk-1..8` | speaker colors | L≈50% | L≈75% |
| `--font-display/body/mono`, `--radius(-sm/-lg)`, `--gap-xs..xl`, `--control-h` | type & shape | | |

Shared classes in `style.css`: `button.primary/.danger/.link`, `.btn`, `.card`, `.badge.<status>` (`recording done asr_error interrupted source off`), `.alert.warn/.error/.ok`, `.page-head`, `.eyebrow`, `.section-label`, `.back-link`, `.mono`, `.clamp-2`, `.progress` (+`.segmented`, `.over`), `.empty`, `.row`, `.spacer`, `.muted`. Reuse them before adding page-local styles.

## Rules (VI + accessibility)

- No hex/rgb/hsl/oklch literals outside the token block of `style.css` (exceptions: `index.html` theme-color, `public/favicon.svg`). Check: `grep -rnE "#[0-9a-fA-F]{3,8}\b|rgb\(|hsl\(" server/web/src` → 0 hits.
- Text contrast ≥ 4.5:1 in **both** themes. Accent text on an `-soft` tint uses `--accent-strong`, not `--accent` (≈4.1–4.4:1, fails).
- Logo: Deep on light, Bean on dark (automatic via `--accent`); never put Bean on white; no stroke/gradient/shadow, no rotation change, never replace the sine slit with bars/mic icon.
- Signal red never appears outside recording state and never inside the logo.
- Times, durations, codes, counters: `.mono` (`font-variant-numeric: tabular-nums`) so live values don't jitter.
- Responsive breakpoint is 767px. At <768px: tap targets ≥ 40px, no horizontal scroll — long unbreakable text needs `min-width: 0; overflow-wrap: anywhere` inside flex rows.
- Record page is desktop-only: at <768px the console is `display:none` (CSS only, component stays mounted so `onBeforeRouteLeave`/unmount cleanup still runs) and a notice is shown.
- The Record console stage is Ink-dark in both themes (VI stage), mark in Bean. Mark state mapping: `recording → rec`; `acquiring/needSys/connecting/reconnecting/stopping → flat`; `idle/saved/interrupted → idle`.

## Common mistakes

- **Hiding focusable controls with `visibility:hidden`** (e.g. hover-revealed "编辑") — they can't receive keyboard focus, so `:focus-visible` never fires. Use `opacity: 0` and reveal on `:hover, :focus-visible`; keep `@media (hover: none)` always-visible for touch.
- **Adding a dark override in only one selector** — forced-dark (`data-theme`) and system-dark (media query) both need it, or one mode silently stays light.
- **Generic `button.link:hover` color winning over `.danger-link`** — danger hover must set `color: var(--danger)` explicitly.

## Validation

- `cd server/web && npm run build` (Windows host: `npm.cmd run build`).
- Leftover-color grep above returns nothing.
- Manual: every page × light/dark × 1280px/375px; theme toggle persists across reload without flash; Record mark states incl. reduced-motion.
