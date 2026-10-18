<template>
  <div>
    <h2 style="margin: 0 0 12px">热词</h2>
    <div class="card">
      <div class="row">
        <strong>额度</strong>
        <span :class="{ over: inactiveCount }">已占用 {{ state.used }} / {{ state.budget }}</span>
        <span class="muted">（估算 token）</span>
        <span class="spacer" />
        <span v-if="inactiveCount" class="muted over">{{ inactiveCount }} 个词超出额度，未生效</span>
      </div>
      <div class="bar"><div class="fill" :style="{ width: usedPct + '%' }" /></div>
      <p class="muted hint">
        笔记和会议共用。识别时按列表从上往下取词，放不下的词标为“未生效”，不会发给识别服务；
        越重要的词请排在越前面。改动只影响之后开始的录音。
      </p>
      <textarea v-model="input" rows="4" placeholder="一行一个词，可一次粘贴多行（每个词 1–32 个字）" />
      <div class="row" style="margin-top: 8px">
        <button class="primary" :disabled="busy || !input.trim()" @click="add">添加</button>
        <span v-if="message" class="muted">{{ message }}</span>
        <span v-if="error" class="error">{{ error }}</span>
      </div>
    </div>

    <div class="card">
      <div v-if="loaded && !state.items.length" class="empty">还没有热词。</div>
      <div v-for="(h, i) in state.items" :key="h.id" class="hw row" :class="{ inactive: !h.active }">
        <span class="muted idx">{{ i + 1 }}</span>
        <template v-if="editingId === h.id">
          <input v-model="draft" class="word-input" maxlength="64" @keyup.enter="save(h)"
                 @keyup.esc="editingId = null" />
          <button class="primary" :disabled="busy" @click="save(h)">保存</button>
          <button @click="editingId = null">取消</button>
        </template>
        <template v-else>
          <span class="word">{{ h.word }}</span>
          <span class="muted">{{ h.tokens }} token</span>
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
.bar { height: 6px; background: #e5e7eb; border-radius: 3px; margin: 10px 0 4px; overflow: hidden; }
.fill { height: 100%; background: #2563eb; }
.over { color: #b45309; }
.hint { margin: 8px 0 10px; line-height: 1.6; }
.hw { padding: 8px 4px; border-bottom: 1px solid #f3f4f6; gap: 10px; }
.hw:last-child { border-bottom: none; }
.hw.inactive .word { color: #9ca3af; }
.idx { width: 24px; text-align: right; }
.word { font-size: 16px; }
.word-input { min-width: 200px; }
.badge.off { background: #f3f4f6; color: #9ca3af; }
.danger-link { color: #b91c1c; }
button.link:disabled { color: #9ca3af; cursor: default; }
</style>
