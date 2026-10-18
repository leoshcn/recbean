<template>
  <div>
    <router-link to="/">← 返回列表</router-link>
    <p v-if="error" class="error">{{ error }}</p>
    <template v-if="rec">
      <div class="card">
        <div class="row">
          <template v-if="editingTitle">
            <input v-model="titleDraft" class="title-input" @keyup.enter="saveTitle"
                   @keyup.esc="editingTitle = false" />
            <button class="primary" @click="saveTitle">保存</button>
            <button @click="editingTitle = false">取消</button>
          </template>
          <template v-else>
            <h2 class="title">{{ rec.title }}</h2>
            <button class="link" @click="startTitle">改标题</button>
          </template>
          <span class="spacer" />
          <a class="btn" :href="api.exportUrl(rec.id)">导出 TXT</a>
          <button class="danger" :disabled="rec.live" @click="remove">删除</button>
        </div>
        <div class="row muted" style="margin-top: 6px">
          <span>{{ fmtTime(rec.started_at) }}</span>
          <span>时长 {{ fmtDuration(rec.duration_ms) }}</span>
          <span class="badge" :class="rec.status">{{ STATUS_TEXT[rec.status] || rec.status }}</span>
        </div>
        <p v-if="rec.error" class="error">识别异常：{{ rec.error }}</p>
        <audio ref="audio" controls preload="metadata" :src="audioSrc" class="player"
               @timeupdate="onTime" />
      </div>

      <div class="card">
        <div class="muted" style="margin-bottom: 8px">
          说话人（改名会同步到整份录音；同一个人被拆成多个时可“合并到”另一位）
          <span v-if="rec.live">· 录制中不能合并</span>
        </div>
        <div class="row">
          <div v-for="s in rec.speakers" :key="s.idx" class="speaker">
            <span class="dot" :style="{ background: speakerColor(s.idx) }" />
            <template v-if="editingSpeaker === s.idx">
              <input v-model="speakerDraft" size="8" @keyup.enter="saveSpeaker(s.idx)"
                     @keyup.esc="editingSpeaker = null" />
              <button class="link" @click="saveSpeaker(s.idx)">保存</button>
            </template>
            <template v-else>
              <span>{{ s.name }}</span>
              <span class="muted">{{ countOf(s.idx) }} 句</span>
              <button class="link" @click="startSpeaker(s)">改名</button>
              <select v-if="rec.speakers.length > 1" class="merge" :disabled="rec.live"
                      title="把该说话人的全部句子并入另一位" @change="mergeSpeaker(s, $event)">
                <option value="" disabled selected>合并到…</option>
                <option v-for="t in rec.speakers.filter((x) => x.idx !== s.idx)" :key="t.idx"
                        :value="t.idx">{{ t.name }}</option>
              </select>
            </template>
          </div>
        </div>
      </div>

      <div class="card">
        <div v-if="!rec.sentences.length" class="empty">暂无转写内容</div>
        <div v-for="s in rec.sentences" :key="s.id" class="sentence"
             :class="{ current: s.id === currentId }">
          <div class="row head">
            <button class="link time" title="从这里播放" @click="seek(s)">
              {{ fmtDuration(s.start_ms) }}
            </button>
            <select :value="s.speaker_idx" :style="{ color: speakerColor(s.speaker_idx) }"
                    @change="changeSpeaker(s, $event.target.value)">
              <option v-for="sp in rec.speakers" :key="sp.idx" :value="sp.idx">{{ sp.name }}</option>
              <option value="new">+ 新说话人</option>
            </select>
            <span v-if="s.edited" class="muted">已编辑</span>
            <span class="spacer" />
            <button v-if="editingId !== s.id" class="link" @click="startEdit(s)">编辑</button>
          </div>
          <div v-if="editingId === s.id">
            <textarea v-model="draft" rows="2" @keydown.enter.exact.prevent="saveEdit(s)"
                      @keyup.esc="editingId = null" />
            <div class="row">
              <button class="primary" @click="saveEdit(s)">保存</button>
              <button @click="editingId = null">取消</button>
              <span class="muted">Enter 保存，Esc 取消</span>
            </div>
          </div>
          <div v-else class="text" @click="seek(s)">{{ s.text }}</div>
        </div>
      </div>
    </template>
  </div>
</template>

<script setup>
import { computed, onMounted, onUnmounted, ref, watch } from 'vue'
import { useRouter } from 'vue-router'
import { api } from '../api'
import { STATUS_TEXT, fmtDuration, fmtTime, speakerColor } from '../util'

const props = defineProps({ id: { type: String, required: true } })
const router = useRouter()
const rec = ref(null)
const error = ref('')
const audio = ref(null)
const audioVersion = ref(0)
const currentMs = ref(-1)
const editingTitle = ref(false)
const titleDraft = ref('')
const editingSpeaker = ref(null)
const speakerDraft = ref('')
const editingId = ref(null)
const draft = ref('')
let timer = null

const audioSrc = computed(() => `${api.audioUrl(props.id)}?v=${audioVersion.value}`)

const currentId = computed(() => {
  if (!rec.value || currentMs.value < 0) return null
  const t = currentMs.value
  const hit = rec.value.sentences.find((s) => t >= s.start_ms && t < s.end_ms)
  return hit ? hit.id : null
})

async function load() {
  try {
    const wasLive = rec.value?.live
    rec.value = await api.getRecording(props.id)
    error.value = ''
    if (wasLive && !rec.value.live) audioVersion.value++ // reload finished WAV
  } catch (e) {
    error.value = e.message === '404' || e.message.includes('not found') ? '录音不存在' : `加载失败：${e.message}`
  }
}

function onTime() {
  currentMs.value = audio.value ? audio.value.currentTime * 1000 : -1
}

function seek(s) {
  const el = audio.value
  if (!el) return
  el.currentTime = s.start_ms / 1000
  el.play().catch(() => {})
}

function startTitle() {
  titleDraft.value = rec.value.title
  editingTitle.value = true
}

async function saveTitle() {
  const t = titleDraft.value.trim()
  if (!t) return
  try {
    const r = await api.updateTitle(props.id, t)
    rec.value.title = r.title
    editingTitle.value = false
  } catch (e) {
    alert(`保存失败：${e.message}`)
  }
}

function startSpeaker(s) {
  speakerDraft.value = s.name
  editingSpeaker.value = s.idx
}

async function saveSpeaker(idx) {
  const name = speakerDraft.value.trim()
  if (!name) return
  try {
    rec.value.speakers = await api.renameSpeaker(props.id, idx, name)
    editingSpeaker.value = null
  } catch (e) {
    alert(`保存失败：${e.message}`)
  }
}

async function changeSpeaker(s, value) {
  let idx = Number(value)
  if (value === 'new') idx = Math.max(0, ...rec.value.speakers.map((x) => x.idx)) + 1
  try {
    await api.updateSentence(props.id, s.id, { speaker_idx: idx })
    await load()
  } catch (e) {
    alert(`保存失败：${e.message}`)
    await load()
  }
}

function countOf(idx) {
  return rec.value.sentences.filter((x) => x.speaker_idx === idx).length
}

async function mergeSpeaker(s, ev) {
  const into = Number(ev.target.value)
  ev.target.value = ''
  const target = rec.value.speakers.find((x) => x.idx === into)
  if (!target) return
  if (!confirm(`把「${s.name}」的 ${countOf(s.idx)} 句合并到「${target.name}」？合并后「${s.name}」将从说话人列表移除。`)) return
  try {
    await api.mergeSpeakers(props.id, s.idx, into)
  } catch (e) {
    alert(`合并失败：${e.message === '409' || e.message.includes('in progress') ? '录音仍在进行中' : e.message}`)
  }
  if (editingSpeaker.value === s.idx) editingSpeaker.value = null
  await load()
}

function startEdit(s) {
  draft.value = s.text
  editingId.value = s.id
}

async function saveEdit(s) {
  try {
    const r = await api.updateSentence(props.id, s.id, { text: draft.value.trim() })
    Object.assign(s, r)
    editingId.value = null
  } catch (e) {
    alert(`保存失败：${e.message}`)
  }
}

async function remove() {
  if (!confirm(`确定删除“${rec.value.title}”？音频和转写将一并删除，无法恢复。`)) return
  try {
    await api.deleteRecording(props.id)
    router.push('/')
  } catch (e) {
    alert(`删除失败：${e.message}`)
  }
}

watch(() => props.id, load)
onMounted(() => {
  load()
  // refresh while the device is still recording (but not while the user is editing)
  timer = setInterval(() => {
    if (rec.value?.live && editingId.value === null && !editingTitle.value && editingSpeaker.value === null) load()
  }, 2000)
})
onUnmounted(() => clearInterval(timer))
</script>

<style scoped>
.title { margin: 0; font-size: 20px; }
.title-input { font-size: 18px; min-width: 280px; }
.player { width: 100%; margin-top: 12px; }
.speaker { display: flex; align-items: center; gap: 6px; padding: 2px 8px; border: 1px solid #e5e7eb; border-radius: 16px; }
.dot { width: 10px; height: 10px; border-radius: 50%; display: inline-block; }
.sentence { padding: 10px 8px; border-bottom: 1px solid #f3f4f6; border-radius: 6px; }
.sentence.current { background: #eff6ff; }
.head { gap: 8px; margin-bottom: 4px; }
.time { font-family: monospace; }
.text { cursor: pointer; line-height: 1.6; white-space: pre-wrap; }
select { padding: 2px 6px; font-weight: 600; }
select.merge { font-weight: normal; font-size: 12px; padding: 1px 4px; }
</style>
