<template>
  <div>
    <router-link to="/notes" class="back-link">← 全部笔记</router-link>
    <header class="page-head">
      <div class="titles">
        <p class="eyebrow">Note · {{ fmtDay(date).slice(date.length + 1) || 'Day' }}</p>
        <h1><span class="mono">{{ date }}</span> 笔记</h1>
        <p v-if="segments.length" class="page-sub mono">
          {{ segments.length }} 段 · 共 {{ fmtDuration(totalMs) }}
        </p>
      </div>
      <div class="actions">
        <button :disabled="!fullText" @click="copyAll">{{ copied ? '已复制' : '复制全文' }}</button>
        <a class="btn" :href="api.noteDayExportUrl(date)">导出 TXT</a>
      </div>
    </header>
    <p v-if="error" class="alert error">{{ error }}</p>
    <div v-if="copyFallback" class="card fallback">
      <div class="muted">浏览器不允许自动复制，请按 Ctrl+C（手机长按）复制下面的内容：</div>
      <textarea ref="fallbackEl" :value="fullText" rows="6" readonly @focus="$event.target.select()" />
      <button @click="copyFallback = false">关闭</button>
    </div>

    <div v-if="loaded && !segments.length" class="card empty">这一天没有笔记。</div>
    <audio ref="audio" preload="none" @ended="playingId = null" />

    <ol v-if="segments.length" class="timeline">
      <li v-for="seg in segments" :key="seg.id" class="seg" :class="{ playing: playingId === seg.id }">
        <div class="rail">
          <span class="time mono">{{ seg.time }}</span>
          <span class="dur mono">{{ fmtDuration(seg.duration_ms) }}</span>
        </div>
        <div class="card seg-card">
          <div class="row head">
            <span v-if="seg.live" class="badge recording">录音中</span>
            <span v-else-if="seg.status !== 'done'" class="badge" :class="seg.status">
              {{ STATUS_TEXT[seg.status] || seg.status }}
            </span>
            <span class="spacer" />
            <button class="link" :disabled="seg.live" @click="togglePlay(seg)">
              {{ playingId === seg.id ? '■ 停止' : '▶ 播放' }}
            </button>
            <button class="link danger-link" :disabled="seg.live" @click="remove(seg)">删除本段</button>
          </div>
          <p v-if="seg.error" class="error small">识别异常：{{ seg.error }}</p>
          <div v-if="!seg.sentences.length" class="muted">（暂无转写）</div>
          <div v-for="s in seg.sentences" :key="s.id" class="sentence">
            <template v-if="editingId === s.id">
              <textarea v-model="draft" rows="2" @keydown.enter.exact.prevent="saveEdit(seg, s)"
                        @keyup.esc="editingId = null" />
              <div class="row edit-actions">
                <button class="primary" @click="saveEdit(seg, s)">保存</button>
                <button @click="editingId = null">取消</button>
                <span class="muted">Enter 保存，Esc 取消</span>
              </div>
            </template>
            <div v-else class="row line">
              <span class="text">{{ s.text }}</span>
              <span v-if="s.edited" class="muted">已编辑</span>
              <button class="link edit" @click="startEdit(s)">编辑</button>
            </div>
          </div>
        </div>
      </li>
    </ol>
  </div>
</template>

<script setup>
import { computed, nextTick, onMounted, onUnmounted, ref, watch } from 'vue'
import { api } from '../api'
import { STATUS_TEXT, fmtDay, fmtDuration, todayStr } from '../util'

const props = defineProps({ date: { type: String, required: true } })
const segments = ref([])
const loaded = ref(false)
const error = ref('')
const audio = ref(null)
const playingId = ref(null)
const editingId = ref(null)
const draft = ref('')
const copied = ref(false)
const copyFallback = ref(false)
const fallbackEl = ref(null)
let timer = null

const totalMs = computed(() => segments.value.reduce((n, s) => n + (s.duration_ms || 0), 0))

function segText(seg) {
  return seg.sentences.map((s) => s.text).join('').trim()
}

// same layout as the server-side TXT export: "HH:MM 文字" per segment
const fullText = computed(() =>
  segments.value
    .filter((seg) => segText(seg))
    .map((seg) => `${seg.time} ${segText(seg)}`)
    .join('\n'),
)

async function load() {
  try {
    const wasLive = new Set(segments.value.filter((s) => s.live).map((s) => s.id))
    const r = await api.getNoteDay(props.date)
    segments.value = r.segments
    error.value = ''
    // a segment that just finished: its WAV header is final now
    if (playingId.value && wasLive.has(playingId.value)) stop()
  } catch (e) {
    error.value = `加载失败：${e.message}`
  } finally {
    loaded.value = true
  }
}

function stop() {
  const el = audio.value
  if (el) el.pause()
  playingId.value = null
}

function togglePlay(seg) {
  const el = audio.value
  if (!el) return
  if (playingId.value === seg.id) {
    stop()
    return
  }
  el.src = `${api.audioUrl(seg.id)}?t=${seg.duration_ms}`
  el.play().then(() => { playingId.value = seg.id }).catch((e) => alert(`播放失败：${e.message}`))
}

function startEdit(s) {
  draft.value = s.text
  editingId.value = s.id
}

async function saveEdit(seg, s) {
  try {
    const r = await api.updateSentence(seg.id, s.id, { text: draft.value.trim() })
    Object.assign(s, r)
    editingId.value = null
  } catch (e) {
    alert(`保存失败：${e.message}`)
  }
}

async function remove(seg) {
  const preview = segText(seg).slice(0, 30)
  if (!confirm(`确定删除 ${seg.time} 这一段笔记？${preview ? `\n“${preview}”` : ''}\n音频和文字将一并删除，无法恢复。`)) return
  try {
    if (playingId.value === seg.id) stop()
    await api.deleteRecording(seg.id)
    await load()
  } catch (e) {
    alert(`删除失败：${e.message}`)
  }
}

async function copyAll() {
  const text = fullText.value
  if (!text) return
  try {
    // navigator.clipboard only exists on https / localhost
    if (!navigator.clipboard || !window.isSecureContext) throw new Error('insecure')
    await navigator.clipboard.writeText(text)
    copied.value = true
    setTimeout(() => { copied.value = false }, 1500)
  } catch {
    copyFallback.value = true
    await nextTick()
    fallbackEl.value?.focus()
    fallbackEl.value?.select()
  }
}

watch(() => props.date, () => {
  stop()
  segments.value = []
  loaded.value = false
  load()
})
onMounted(() => {
  load()
  let ticks = 0
  timer = setInterval(() => {
    ticks += 1
    if (editingId.value !== null) return
    // notes are seconds long, so a new one is rarely caught "live": keep polling today's
    // page slowly (every 9 s) even when nothing is recording
    if (segments.value.some((s) => s.live) || (props.date === todayStr() && ticks % 3 === 0)) load()
  }, 3000)
})
onUnmounted(() => {
  clearInterval(timer)
  stop()
})
</script>

<style scoped>
.timeline { list-style: none; margin: 0; padding: 0; }
.seg {
  position: relative; display: grid; grid-template-columns: 76px minmax(0, 1fr); gap: var(--gap-sm);
}
/* vertical rail line + node */
.seg::before {
  content: ''; position: absolute; left: 82px; top: 0; bottom: 0; width: 1px; background: var(--border);
}
.seg::after {
  content: ''; position: absolute; left: 79px; top: 22px; width: 7px; height: 7px; border-radius: 50%;
  background: var(--surface); border: 1px solid var(--muted);
}
.seg.playing::after { background: var(--accent); border-color: var(--accent); }
.rail { display: flex; flex-direction: column; align-items: flex-end; padding: 14px 10px 0 0; }
.time { font-size: 16px; font-weight: 600; }
.seg.playing .time { color: var(--accent); }
.dur { font-size: 12px; color: var(--muted); }
.seg-card { margin-left: 14px; padding-block: 12px 14px; }
.head { gap: 6px; margin-bottom: 4px; min-height: 28px; }
.sentence { padding: 3px 0; }
.line { gap: 8px; flex-wrap: nowrap; align-items: baseline; }
.text { line-height: 1.75; white-space: pre-wrap; flex: 1; min-width: 0; overflow-wrap: anywhere; }
/* opacity (not visibility) so the button stays reachable by keyboard */
.edit { opacity: 0; flex-shrink: 0; }
.sentence:hover .edit, .edit:focus-visible { opacity: 1; }
@media (hover: none) { .edit { opacity: 1; } }
.edit-actions { margin-top: 6px; gap: 8px; }
.fallback { display: flex; flex-direction: column; gap: 8px; align-items: flex-start; }
.small { font-size: 13px; margin: 4px 0; }
@media (max-width: 767px) {
  .seg { grid-template-columns: 1fr; gap: 0; }
  .seg::before, .seg::after { display: none; }
  .rail { flex-direction: row; align-items: baseline; gap: 8px; padding: 8px 2px 6px; }
  .seg-card { margin-left: 0; }
}
</style>
