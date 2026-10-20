// Theme preference: 'system' follows prefers-color-scheme (pure CSS, see style.css);
// 'light' / 'dark' force it via <html data-theme>. index.html applies the stored value
// inline before first paint; keep the key and values in sync with that script.
const KEY = 'recbean-theme'
export const THEMES = ['system', 'light', 'dark']

export function getTheme() {
  try {
    const v = localStorage.getItem(KEY)
    return THEMES.includes(v) ? v : 'system'
  } catch {
    return 'system'
  }
}

export function setTheme(mode) {
  const m = THEMES.includes(mode) ? mode : 'system'
  try {
    if (m === 'system') localStorage.removeItem(KEY)
    else localStorage.setItem(KEY, m)
  } catch {
    /* storage disabled: the choice just lasts for this page */
  }
  applyTheme(m)
  return m
}

export function applyTheme(mode) {
  const root = document.documentElement
  if (mode === 'light' || mode === 'dark') root.dataset.theme = mode
  else delete root.dataset.theme
  syncThemeColor(mode)
}

// index.html has one <meta name="theme-color"> per color scheme (media attribute).
// Forced mode: point both at the forced scheme's color; 'system': restore originals.
function syncThemeColor(mode) {
  const metas = [...document.querySelectorAll('meta[name="theme-color"]')]
  for (const m of metas) {
    if (!m.dataset.color) m.dataset.color = m.content
  }
  const forced = metas.find((m) => (m.media || '').includes(mode))
  for (const m of metas) {
    m.content = forced && mode !== 'system' ? forced.dataset.color : m.dataset.color
  }
}
