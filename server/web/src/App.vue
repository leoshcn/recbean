<template>
  <div class="shell" :class="{ 'drawer-open': drawerOpen }">
    <!-- mobile (< 768px): narrow top bar; the sidebar becomes a drawer -->
    <header class="mobile-bar">
      <router-link to="/notes" class="lockup" aria-label="录音豆 首页">
        <BrandMark :size="22" />
        <span class="wordmark">录音豆</span>
      </router-link>
      <button class="icon-btn" type="button" aria-controls="app-nav" :aria-expanded="drawerOpen"
              :aria-label="drawerOpen ? '关闭菜单' : '打开菜单'" @click="drawerOpen = !drawerOpen">
        <svg viewBox="0 0 24 24" aria-hidden="true">
          <path v-if="drawerOpen" d="M6 6l12 12M18 6 6 18" />
          <path v-else d="M4 7h16M4 12h16M4 17h16" />
        </svg>
      </button>
    </header>
    <div v-if="drawerOpen" class="backdrop" @click="drawerOpen = false" />

    <aside id="app-nav" class="sidebar" aria-label="主导航">
      <router-link to="/notes" class="lockup brand" aria-label="录音豆 首页">
        <BrandMark :size="28" />
        <span class="wordmark">录音豆</span>
      </router-link>

      <nav class="nav">
        <router-link v-for="item in NAV" :key="item.to" :to="item.to" class="nav-item"
                     :class="{ active: isActive(item) }">
          <svg class="nav-icon" viewBox="0 0 24 24" aria-hidden="true" v-html="item.icon" />
          <span>{{ item.label }}</span>
        </router-link>
      </nav>

      <div class="sidebar-foot">
        <div class="theme-toggle" role="group" aria-label="外观主题">
          <button v-for="t in THEME_OPTS" :key="t.value" type="button" class="theme-btn"
                  :class="{ on: theme === t.value }" :aria-pressed="theme === t.value"
                  :title="t.label" :aria-label="t.label" @click="chooseTheme(t.value)">
            <svg viewBox="0 0 24 24" aria-hidden="true" v-html="t.icon" />
          </button>
        </div>
        <span class="foot-meta mono">RECBEAN</span>
      </div>
    </aside>

    <main class="main">
      <div class="container">
        <router-view />
      </div>
    </main>
  </div>
</template>

<script setup>
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { useRoute } from 'vue-router'
import BrandMark from './components/BrandMark.vue'
import { applyTheme, getTheme, setTheme } from './theme'

const NAV = [
  { to: '/notes', label: '笔记',
    icon: '<path d="M6 3h8.5L19 7.5V21H6z"/><path d="M14 3v5h5M9 12.5h7M9 16.5h5"/>' },
  { to: '/meetings', label: '会议', meetings: true,
    icon: '<circle cx="9" cy="8" r="3.2"/><path d="M3 20c0-3.3 2.7-6 6-6s6 2.7 6 6"/><path d="M15.5 5.2a3 3 0 0 1 0 5.6M17 14.3c2.4.6 4 2.9 4 5.7"/>' },
  { to: '/record', label: '浏览器录音',
    icon: '<circle cx="12" cy="12" r="8.5"/><circle cx="12" cy="12" r="3.5" class="fill"/>' },
  { to: '/hotwords', label: '热词',
    icon: '<path d="M5 9h15M4 15h15M10.5 4 8.5 20M15.5 4l-2 16"/>' },
  { to: '/pairing', label: '设备配对',
    icon: '<path d="M10 14a4 4 0 0 0 5.66 0l3-3a4 4 0 0 0-5.66-5.66l-1 1"/><path d="M14 10a4 4 0 0 0-5.66 0l-3 3a4 4 0 0 0 5.66 5.66l1-1"/>' },
]

const THEME_OPTS = [
  { value: 'system', label: '跟随系统',
    icon: '<rect x="3" y="4" width="18" height="12" rx="2"/><path d="M8 20h8M12 16v4"/>' },
  { value: 'light', label: '浅色',
    icon: '<circle cx="12" cy="12" r="4"/><path d="M12 2.5v2M12 19.5v2M2.5 12h2M19.5 12h2M5.3 5.3l1.4 1.4M17.3 17.3l1.4 1.4M5.3 18.7l1.4-1.4M17.3 6.7l1.4-1.4"/>' },
  { value: 'dark', label: '深色',
    icon: '<path d="M20 14.5A8 8 0 1 1 9.5 4a6.5 6.5 0 0 0 10.5 10.5z"/>' },
]

const route = useRoute()
// meeting details live under /r/:id
const inMeetings = computed(() => route.path.startsWith('/meetings') || route.path.startsWith('/r/'))

function isActive(item) {
  return item.meetings ? inMeetings.value : route.path.startsWith(item.to)
}

const theme = ref(getTheme())
applyTheme(theme.value)
function chooseTheme(v) {
  theme.value = setTheme(v)
}

const drawerOpen = ref(false)
watch(() => route.fullPath, () => { drawerOpen.value = false })
function onKey(e) {
  if (e.key === 'Escape' && drawerOpen.value) drawerOpen.value = false
}
onMounted(() => window.addEventListener('keydown', onKey))
onBeforeUnmount(() => window.removeEventListener('keydown', onKey))
</script>

<style scoped>
.shell {
  display: grid; grid-template-columns: 228px minmax(0, 1fr); min-height: 100vh;
}

/* ---- sidebar ---- */
.sidebar {
  position: sticky; top: 0; height: 100vh; overflow-y: auto;
  display: flex; flex-direction: column; gap: var(--gap-md);
  padding: var(--gap-md) var(--gap-sm);
  background: var(--surface-2); border-right: 1px solid var(--border);
}
.lockup {
  display: inline-flex; align-items: center; gap: 10px; color: var(--fg);
  border-radius: var(--radius-sm);
}
.lockup:hover { text-decoration: none; }
.brand { padding: 6px 10px 10px; }
.wordmark {
  font-family: var(--font-display); font-weight: 600; font-size: 19px; letter-spacing: 0.06em; line-height: 1;
}

.nav { display: flex; flex-direction: column; gap: 2px; }
.nav-item {
  display: flex; align-items: center; gap: 10px; min-height: 40px; padding: 8px 12px;
  border-radius: var(--radius); color: var(--muted); font-size: 14px; font-weight: 500;
  transition: background .15s, color .15s;
}
.nav-item:hover { color: var(--fg); background: var(--fg-soft); text-decoration: none; }
.nav-item.active { color: var(--accent-strong); background: var(--accent-soft); }
.nav-icon {
  width: 18px; height: 18px; flex: none;
  fill: none; stroke: currentColor; stroke-width: 1.7; stroke-linecap: round; stroke-linejoin: round;
}
.nav-icon :deep(.fill) { fill: currentColor; stroke: none; }

.sidebar-foot {
  margin-top: auto; display: flex; align-items: center; justify-content: space-between; gap: 8px;
  padding: 0 4px;
}
.foot-meta { font-size: 10px; letter-spacing: 0.14em; color: var(--muted); }
.theme-toggle {
  display: inline-flex; gap: 2px; padding: 3px;
  border: 1px solid var(--border); border-radius: var(--radius); background: var(--surface);
}
.theme-btn {
  min-height: 0; width: 32px; height: 30px; padding: 0;
  border: none; border-radius: 7px; background: transparent; color: var(--muted);
}
.theme-btn:hover { background: var(--fg-soft); color: var(--fg); }
.theme-btn.on { background: var(--accent-soft); color: var(--accent-strong); }
.theme-btn svg, .icon-btn svg {
  width: 17px; height: 17px;
  fill: none; stroke: currentColor; stroke-width: 1.7; stroke-linecap: round; stroke-linejoin: round;
}

/* ---- main ---- */
.main { min-width: 0; }
.container { max-width: 960px; margin: 0 auto; padding: var(--gap-lg) var(--gap-lg) var(--gap-xl); }

/* ---- mobile ---- */
.mobile-bar, .backdrop { display: none; }
.icon-btn {
  width: 40px; height: 40px; min-height: 40px; padding: 0;
  border: none; background: transparent; color: var(--fg);
}

@media (max-width: 767px) {
  .shell { display: block; }
  .mobile-bar {
    position: sticky; top: 0; z-index: 30;
    display: flex; align-items: center; justify-content: space-between;
    height: 52px; padding: 0 6px 0 14px;
    background: var(--surface-2); border-bottom: 1px solid var(--border);
  }
  .mobile-bar .wordmark { font-size: 17px; }
  .sidebar {
    position: fixed; z-index: 50; top: 0; left: 0; bottom: 0; height: auto;
    width: min(280px, 82vw);
    transform: translateX(-100%); visibility: hidden;
    transition: transform .2s ease, visibility 0s linear .2s;
  }
  .drawer-open .sidebar {
    transform: none; visibility: visible;
    transition: transform .2s ease, visibility 0s;
  }
  .backdrop {
    display: block; position: fixed; inset: 0; z-index: 40;
    background: color-mix(in oklch, var(--ink) 45%, transparent);
  }
  .nav-item { min-height: 44px; font-size: 15px; }
  .theme-btn { width: 40px; height: 40px; }
  .container { padding: var(--gap-md) 14px var(--gap-lg); }
}
</style>
