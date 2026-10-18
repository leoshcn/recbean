<template>
  <div>
    <div class="row" style="margin-bottom: 12px">
      <h2 style="margin: 0">笔记</h2>
      <span class="spacer" />
      <button @click="load">刷新</button>
    </div>
    <p v-if="error" class="error">加载失败：{{ error }}</p>
    <div v-if="loaded && !days.length" class="card empty">
      还没有笔记。在设备待机页按住“上”键说话。
    </div>
    <div v-for="d in days" :key="d.date" class="card item" @click="open(d)">
      <div class="row">
        <strong class="title">{{ fmtDay(d.date) }}</strong>
        <span v-if="d.live" class="badge recording">录音中</span>
        <span class="spacer" />
        <span class="muted">{{ d.count }} 段 · 共 {{ fmtDuration(d.duration_ms) }}</span>
      </div>
      <div class="first">{{ d.first_text || '（暂无转写）' }}</div>
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
.item { cursor: pointer; }
.item:hover { border-color: #93c5fd; }
.title { font-size: 16px; }
.first {
  margin-top: 8px; color: #374151;
  white-space: nowrap; overflow: hidden; text-overflow: ellipsis;
}
</style>
