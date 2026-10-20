<template>
  <div>
    <header class="page-head">
      <div class="titles">
        <p class="eyebrow">Meetings</p>
        <h1>会议录音</h1>
        <p v-if="items.length" class="page-sub mono">{{ items.length }} 条录音</p>
      </div>
      <div class="actions">
        <button @click="load">刷新</button>
      </div>
    </header>
    <p v-if="error" class="alert error">加载失败：{{ error }}</p>
    <div v-if="loaded && !items.length" class="card empty">
      还没有会议录音。在设备上单击“确定”开始，或使用<router-link to="/record">浏览器录音</router-link>。
    </div>
    <div v-for="r in items" :key="r.id" class="card item rec" tabindex="0" @click="open(r)"
         @keydown.enter.self="open(r)">
      <div class="row top">
        <strong class="title">{{ r.title }}</strong>
        <span class="badge" :class="r.status">{{ STATUS_TEXT[r.status] || r.status }}</span>
        <span v-if="r.device_id === BROWSER_DEVICE" class="badge source">浏览器</span>
        <span class="spacer" />
        <button class="danger" :disabled="r.live" @click.stop="remove(r)">删除</button>
      </div>
      <div class="meta mono">
        <span>{{ fmtTime(r.started_at) }}</span>
        <span>时长 {{ fmtDuration(r.duration_ms) }}</span>
        <span>{{ r.speaker_count }} 位说话人</span>
        <span>{{ r.sentence_count }} 句</span>
      </div>
      <div class="first clamp-2">{{ r.first_sentence || '（暂无转写）' }}</div>
    </div>
  </div>
</template>

<script setup>
import { onMounted, onUnmounted, ref } from 'vue'
import { useRouter } from 'vue-router'
import { api } from '../api'
import { BROWSER_DEVICE, STATUS_TEXT, fmtDuration, fmtTime } from '../util'

const router = useRouter()
const items = ref([])
const loaded = ref(false)
const error = ref('')
let timer = null

async function load() {
  try {
    items.value = await api.listRecordings('meeting')
    error.value = ''
  } catch (e) {
    error.value = e.message
  } finally {
    loaded.value = true
  }
}

function open(r) {
  router.push(`/r/${encodeURIComponent(r.id)}`)
}

async function remove(r) {
  if (!confirm(`确定删除“${r.title}”？音频和转写将一并删除，无法恢复。`)) return
  try {
    await api.deleteRecording(r.id)
    await load()
  } catch (e) {
    alert(`删除失败：${e.message}`)
  }
}

onMounted(() => {
  load()
  timer = setInterval(() => {
    if (items.value.some((r) => r.live)) load()
  }, 3000)
})
onUnmounted(() => clearInterval(timer))
</script>

<style scoped>
.top { gap: 8px; }
.title { font-size: 16px; font-weight: 600; min-width: 0; overflow-wrap: anywhere; }
.meta {
  display: flex; flex-wrap: wrap; gap: 4px 16px; margin-top: 6px;
  font-size: 12px; color: var(--muted);
}
.meta span + span::before { content: '/'; margin-right: 16px; opacity: .45; }
.first { margin-top: 8px; color: var(--fg); line-height: 1.6; }
@media (max-width: 767px) {
  .meta span + span::before { display: none; }
  .meta { gap: 2px 12px; }
}
</style>
