<template>
  <div>
    <header class="page-head">
      <div class="titles">
        <p class="eyebrow">Notes</p>
        <h1>笔记</h1>
        <p v-if="days.length" class="page-sub mono">{{ days.length }} 天</p>
      </div>
      <div class="actions">
        <button @click="load">刷新</button>
      </div>
    </header>
    <p v-if="error" class="alert error">加载失败：{{ error }}</p>
    <div v-if="loaded && !days.length" class="card empty">
      还没有笔记。在设备待机页按住“上”键说话。
    </div>
    <div v-for="d in days" :key="d.date" class="card item day" tabindex="0" @click="open(d)"
         @keydown.enter.self="open(d)">
      <div class="when">
        <span class="date mono">{{ d.date }}</span>
        <span class="wd">{{ fmtDay(d.date).slice(d.date.length + 1) }}</span>
      </div>
      <div class="body">
        <div class="row meta">
          <span v-if="d.live" class="badge recording">录音中</span>
          <span class="mono">{{ d.count }} 段</span>
          <span class="sep">·</span>
          <span class="mono">{{ fmtDuration(d.duration_ms) }}</span>
        </div>
        <div class="first">{{ d.first_text || '（暂无转写）' }}</div>
      </div>
      <span class="chev" aria-hidden="true">›</span>
    </div>
  </div>
</template>

<script setup>
import { onMounted, onUnmounted, ref } from 'vue'
import { useRouter } from 'vue-router'
import { api } from '../api'
import { fmtDay, fmtDuration } from '../util'

const router = useRouter()
const days = ref([])
const loaded = ref(false)
const error = ref('')
let timer = null

async function load() {
  try {
    days.value = await api.listNoteDays()
    error.value = ''
  } catch (e) {
    error.value = e.message
  } finally {
    loaded.value = true
  }
}

function open(d) {
  router.push(`/notes/${d.date}`)
}

onMounted(() => {
  load()
  let ticks = 0
  timer = setInterval(() => {
    ticks += 1
    // short notes are rarely caught "live": also refresh slowly (every 9 s) when idle
    if (days.value.some((d) => d.live) || ticks % 3 === 0) load()
  }, 3000)
})
onUnmounted(() => clearInterval(timer))
</script>

<style scoped>
.day {
  display: grid; grid-template-columns: 132px minmax(0, 1fr) auto; align-items: center;
  gap: var(--gap-md); padding-block: 16px;
}
.when { display: flex; flex-direction: column; gap: 2px; }
.date { font-size: 16px; font-weight: 600; letter-spacing: -0.01em; }
.wd { font-size: var(--fs-meta); color: var(--muted); }
.meta { gap: 6px; font-size: var(--fs-meta); color: var(--muted); }
.sep { opacity: .6; }
.first {
  margin-top: 4px; color: var(--fg);
  white-space: nowrap; overflow: hidden; text-overflow: ellipsis;
}
.chev { font-size: 22px; line-height: 1; color: var(--muted); transition: transform .15s, color .15s; }
.day:hover .chev { color: var(--accent); transform: translateX(2px); }
@media (max-width: 767px) {
  .day { grid-template-columns: minmax(0, 1fr) auto; gap: 6px 12px; }
  .when { flex-direction: row; align-items: baseline; gap: 8px; grid-column: 1; }
  .body { grid-column: 1; }
  .chev { grid-column: 2; grid-row: 1 / span 2; }
}
</style>
