<template>
  <div>
    <header class="page-head">
      <div class="titles">
        <p class="eyebrow">Hotwords</p>
        <h1>热词</h1>
        <p v-if="state.items.length" class="page-sub mono">{{ state.items.length }} 个词</p>
      </div>
    </header>
    <div class="card">
      <div class="row quota">
        <span class="section-label">额度</span>
        <span class="used mono" :class="{ over: inactiveCount }">
          {{ state.used }}<span class="of"> / {{ state.budget }}</span>
        </span>
        <span class="muted">估算 token</span>
        <span class="spacer" />
        <span v-if="inactiveCount" class="muted over">{{ inactiveCount }} 个词超出额度，未生效</span>
      </div>
      <div class="progress bar" :class="{ over: inactiveCount }"><span :style="{ width: usedPct + '%' }" /></div>
      <p class="muted hint">
        笔记和会议共用。识别时按列表从上往下取词，放不下的词标为“未生效”，不会发给识别服务；
        越重要的词请排在越前面。改动只影响之后开始的录音。
      </p>
      <textarea v-model="input" rows="4" placeholder="一行一个词，可一次粘贴多行（每个词 1–32 个字）" />
      <div class="row add-row">
        <button class="primary" :disabled="busy || !input.trim()" @click="add">添加</button>
        <span v-if="message" class="muted">{{ message }}</span>
        <span v-if="error" class="error">{{ error }}</span>
      </div>
    </div>

    <div class="card list">
      <div v-if="loaded && !state.items.length" class="empty">还没有热词。</div>
      <div v-for="(h, i) in state.items" :key="h.id" class="hw row" :class="{ inactive: !h.active }">
        <span class="idx mono">{{ String(i + 1).padStart(2, '0') }}</span>
        <template v-if="editingId === h.id">
          <input v-model="draft" class="word-input" maxlength="64" @keyup.enter="save(h)"
                 @keyup.esc="editingId = null" />
          <button class="primary" :disabled="busy" @click="save(h)">保存</button>
          <button @click="editingId = null">取消</button>
        </template>
        <template v-else>
          <span class="word">{{ h.word }}</span>
          <span class="muted mono">{{ h.tokens }} token</span>
          <span class="badge" :class="h.active ? 'done' : 'off'">{{ h.active ? '生效' : '未生效' }}</span>
          <span class="spacer" />
          <button class="link" :disabled="busy || i === 0" title="上移" @click="move(i, -1)">上移</button>
          <button class="link" :disabled="busy || i === state.items.length - 1" title="下移"
                  @click="move(i, 1)">下移</button>
          <button class="link" @click="startEdit(h)">编辑</button>
          <button class="link danger-link" :disabled="busy" @click="remove(h)">删除</button>
        </template>
      </div>
    </div>
  </div>
</template>

<script setup>
import { computed, onMounted, ref } from 'vue'
import { api } from '../api'

const MAX_CHARS = 32
const state = ref({ budget: 0, used: 0, items: [] })
const loaded = ref(false)
const input = ref('')
const message = ref('')
const error = ref('')
const busy = ref(false)
const editingId = ref(null)
const draft = ref('')

const inactiveCount = computed(() => state.value.items.filter((h) => !h.active).length)
const usedPct = computed(() =>
  state.value.budget ? Math.min(100, (100 * state.value.used) / state.value.budget) : 0,
)

function setState(s) {
  state.value = { budget: s.budget, used: s.used, items: s.items }
}

async function load() {
  try {
    setState(await api.listHotwords())
    error.value = ''
  } catch (e) {
    error.value = `加载失败：${e.message}`
  } finally {
    loaded.value = true
  }
}

// [...str].length counts characters the same way as the server (code points)
const charLen = (w) => [...w].length

async function add() {
  message.value = ''
  error.value = ''
  const words = input.value.split(/\r?\n/).map((w) => w.trim()).filter(Boolean)
  if (!words.length) return
  const tooLong = words.filter((w) => charLen(w) > MAX_CHARS)
  if (tooLong.length) {
    error.value = `每个词最多 ${MAX_CHARS} 个字：${tooLong[0].slice(0, 20)}…`
    return
  }
  busy.value = true
  try {
    const r = await api.addHotwords(words)
    setState(r)
    input.value = ''
    message.value = `已添加 ${r.added} 个` + (r.skipped ? `，跳过 ${r.skipped} 个重复的词` : '')
  } catch (e) {
    error.value = `添加失败：${e.message}`
  } finally {
    busy.value = false
  }
}

function startEdit(h) {
  draft.value = h.word
  editingId.value = h.id
}

async function save(h) {
  const w = draft.value.trim()
  if (!w) {
    alert('热词不能为空')
    return
  }
  if (charLen(w) > MAX_CHARS) {
    alert(`每个词最多 ${MAX_CHARS} 个字`)
    return
  }
  busy.value = true
  try {
    setState(await api.updateHotword(h.id, w))
    editingId.value = null
  } catch (e) {
    alert(`保存失败：${e.message}`)
  } finally {
    busy.value = false
  }
}

async function remove(h) {
  if (!confirm(`删除热词“${h.word}”？`)) return
  busy.value = true
  try {
    setState(await api.deleteHotword(h.id))
  } catch (e) {
    alert(`删除失败：${e.message}`)
    await load()
  } finally {
    busy.value = false
  }
}

async function move(i, delta) {
  const ids = state.value.items.map((h) => h.id)
  const j = i + delta
  if (j < 0 || j >= ids.length) return
  ;[ids[i], ids[j]] = [ids[j], ids[i]]
  busy.value = true
  try {
    setState(await api.reorderHotwords(ids))
  } catch (e) {
    alert(`排序失败：${e.message}`)
    await load()
  } finally {
    busy.value = false
  }
}

onMounted(load)
</script>

<style scoped>
.quota { gap: 10px; align-items: baseline; }
.quota .section-label { margin: 0; }
.used { font-size: 22px; font-weight: 600; letter-spacing: -0.01em; }
.of { font-size: 14px; font-weight: 400; color: var(--muted); }
.over { color: var(--warn); }
.bar { margin: 10px 0 4px; }
.hint { margin: 10px 0 12px; line-height: 1.7; }
.add-row { margin-top: 10px; }
.list { padding-block: 6px; }
.hw { padding: 8px 4px; border-bottom: 1px solid var(--border); gap: 10px; min-height: 52px; }
.hw:last-child { border-bottom: none; }
.hw.inactive .word { color: var(--muted); text-decoration: line-through; text-decoration-color: var(--border-strong); }
.idx { width: 26px; text-align: right; font-size: 12px; color: var(--muted); }
.word { font-size: 16px; overflow-wrap: anywhere; min-width: 0; }
.word-input { min-width: 200px; flex: 1; }
@media (max-width: 767px) {
  .word-input { min-width: 0; width: 100%; }
  .hw { gap: 6px 8px; }
}
</style>
