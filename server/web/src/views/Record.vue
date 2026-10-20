<template>
  <div>
    <div class="row" style="margin-bottom: 12px">
      <h2 style="margin: 0">浏览器录音</h2>
      <span class="spacer" />
      <span v-if="connText" class="badge" :class="connClass">{{ connText }}</span>
    </div>

    <div v-if="!secure" class="card warn">
      请通过 HTTPS（群晖反代地址）访问本页面。浏览器只允许在安全连接下使用麦克风和屏幕共享，
      当前地址 {{ origin }} 无法录音。
    </div>

    <div class="card">
      <div class="row">
        <label class="opt">
          <input v-model="useMic" type="checkbox" :disabled="busy" /> 麦克风
        </label>
        <select v-model="micId" :disabled="busy || !useMic" class="mic-select">
          <option value="">默认麦克风</option>
          <option v-for="m in mics" :key="m.id" :value="m.id">{{ m.label }}</option>
        </select>
        <label class="opt">
          <input v-model="useSys" type="checkbox" :disabled="busy || !sysSupported" /> 系统声音
        </label>
      </div>
      <p v-if="secure && !sysSupported" class="muted">
        当前浏览器不支持共享系统声音，只能录制麦克风。请在 Windows 上使用 Chrome 或 Edge。
      </p>
      <p v-else-if="useSys && !busy" class="muted">
        开始后在弹窗中选择“整个屏幕”，并勾选“共享系统音频”。建议佩戴耳机，避免外放的声音被麦克风重复录入。
      </p>

      <div v-if="state === 'needSys'" class="card warn prompt">
        <div>未获取到系统声音（取消了共享，或共享时没有选择“整个屏幕”并勾选“共享系统音频”）。</div>
        <div class="row" style="margin-top: 8px">
          <button class="primary" @click="pickSystem">重新选择共享</button>
          <button v-if="micStream" @click="continueMicOnly">仅录制麦克风</button>
          <button @click="cancelAcquire">取消</button>
        </div>
      </div>

      <div class="row controls">
        <button v-if="!busy" class="primary big" :disabled="!canStart" @click="start">开始录音</button>
        <button v-else class="danger big" :disabled="state === 'stopping' || state === 'acquiring' || state === 'needSys'"
                @click="stop">
          {{ state === 'stopping' ? '正在保存…' : '停止并保存' }}
        </button>
        <span class="duration">{{ fmtDuration(durationMs) }}</span>
        <div class="levels">
          <div class="level-row" :class="{ off: !hasMic }">
            <span class="muted">麦克风</span>
            <span class="meter"><span :style="{ width: pct(level.mic) }" /></span>
          </div>
          <div class="level-row" :class="{ off: !hasSys }">
            <span class="muted">系统声音</span>
            <span class="meter"><span :style="{ width: pct(level.sys) }" /></span>
          </div>
        </div>
      </div>

      <p v-for="(w, i) in warnings" :key="i" class="warn-line">{{ w }}</p>
      <p v-if="error" class="error">{{ error }}</p>
      <p v-if="state === 'saved'" class="ok">
        已保存。<router-link :to="`/r/${encodeURIComponent(recId)}`">查看录音详情 →</router-link>
      </p>
      <p v-if="state === 'interrupted' && recId" class="error">
        录音已中断并保存。<router-link :to="`/r/${encodeURIComponent(recId)}`">查看录音详情 →</router-link>
      </p>
    </div>

    <div class="card">
      <div class="muted" style="margin-bottom: 8px">实时转写</div>
      <div ref="list" class="transcript">
        <div v-if="!finals.length && !partial" class="empty">{{ busy ? '等待语音…' : '开始录音后在这里显示转写' }}</div>
        <div v-for="s in finals" :key="s.idx" class="sentence">
          <span class="spk" :style="{ color: speakerColor(s.speaker) }">说话人{{ s.speaker }}</span>
          <span class="muted time">{{ fmtDuration(s.start_ms) }}</span>
          <div>{{ s.text }}</div>
        </div>
        <div v-if="partial" class="sentence partial">{{ partial }}</div>
      </div>
    </div>
  </div>
</template>

<script setup>
import { computed, nextTick, onBeforeUnmount, onMounted, reactive, ref, watch } from 'vue'
import { onBeforeRouteLeave } from 'vue-router'
import {
  FRAME_MS, Recorder, canCaptureMic, canCaptureSystem, isSecure, listMics, openMic,
  openSystemAudio, stopStream,
} from '../recorder'
import { fmtDuration, speakerColor } from '../util'

const RING_FRAMES = 300 // 60 s of 200 ms frames kept for resend after a reconnect
const BACKOFF_MS = [1000, 2000, 4000, 5000]
const CLIENT = 'web/0.1.0'
const FRAME_BYTES = 6404 // seq u32 + 3200 s16le samples
const STALL_BYTES = 50 * FRAME_BYTES // ~10 s of unsent audio queued in the socket...
const STALL_MS = 10000 // ...that has not drained at all for this long: treat it as dead

// idle → acquiring (→ needSys) → connecting → recording ⇄ reconnecting → stopping → saved | interrupted
const state = ref('idle')
const secure = isSecure()
const sysSupported = canCaptureSystem()
const origin = location.origin
const useMic = ref(true)
const useSys = ref(sysSupported)
const micId = ref('')
const mics = ref([])
const level = reactive({ mic: 0, sys: 0 })
const hasMic = ref(false)
const hasSys = ref(false)
const frameCount = ref(0)
const recId = ref('')
const finals = ref([])
const partial = ref('')
const warnings = ref([])
const error = ref('')
const list = ref(null)

let micStream = null
let sysStream = null
let recorder = null
let ws = null
let resumeToken = ''
let ring = [] // [{ seq, buf }] oldest first
let nextSeq = 0
let ready = false // server accepted start/resume on the current ws: frames may be sent
let stopRequested = false
let attempt = 0
let retryTimer = null
let stallSince = 0 // when bufferedAmount stopped draining while above STALL_BYTES
let lastBuffered = 0
let unmounted = false

const busy = computed(() => !['idle', 'saved', 'interrupted'].includes(state.value))
const canStart = computed(() => secure && canCaptureMic() && (useMic.value || useSys.value))
const durationMs = computed(() => frameCount.value * FRAME_MS)
const connText = computed(() => ({
  connecting: '连接中', recording: '已连接', stopping: '已连接', reconnecting: '重连中',
  interrupted: '已中断',
})[state.value] || '')
const connClass = computed(() => ({
  recording: 'done', stopping: 'done', reconnecting: 'asr_error', interrupted: 'interrupted',
})[state.value] || '')

function pct(v) {
  // peak → rough dB-ish scale so normal speech moves the bar visibly
  const db = 20 * Math.log10(Math.max(v, 1e-4))
  return `${Math.max(0, Math.min(100, ((db + 60) / 60) * 100)).toFixed(0)}%`
}

function warn(msg) {
  if (!warnings.value.includes(msg)) warnings.value.push(msg)
}

async function refreshMics() {
  try {
    mics.value = await listMics()
  } catch {
    mics.value = []
  }
}

// ---- capture ---------------------------------------------------------------

async function start() {
  error.value = ''
  warnings.value = []
  finals.value = []
  partial.value = ''
  recId.value = ''
  frameCount.value = 0
  state.value = 'acquiring'
  if (useMic.value) {
    try {
      micStream = await openMic(micId.value)
      if (unmounted) return releaseCapture()
      refreshMics()
    } catch (e) {
      fail(`无法打开麦克风：${e.message || e.name}`)
      return
    }
  }
  if (useSys.value) await pickSystem()
  else begin()
}

// Called from start() and from the "重新选择共享" button (needs the click's user gesture).
async function pickSystem() {
  state.value = 'acquiring'
  try {
    sysStream = await openSystemAudio()
    if (unmounted) return releaseCapture()
  } catch (e) {
    if (e.name === 'NotAllowedError') {
      // user closed the share dialog
      if (micStream) {
        state.value = 'needSys'
        return
      }
      fail('已取消屏幕共享。')
    } else {
      fail(`无法共享系统声音：${e.message || e.name}`)
    }
    return
  }
  if (!sysStream) {
    if (micStream) {
      state.value = 'needSys'
      return
    }
    fail('未获取到系统声音。请重新开始，在弹窗中选择“整个屏幕”并勾选“共享系统音频”。')
    return
  }
  begin()
}

function continueMicOnly() {
  warn('未获取到系统声音，仅录制麦克风。')
  begin()
}

function cancelAcquire() {
  releaseCapture()
  state.value = 'idle'
}

function releaseCapture() {
  if (recorder) recorder.stop()
  recorder = null
  stopStream(micStream)
  stopStream(sysStream)
  micStream = sysStream = null
  hasMic.value = hasSys.value = false
  level.mic = level.sys = 0
}

function fail(msg) {
  error.value = msg
  releaseCapture()
  closeWs()
  state.value = 'idle'
}

async function begin() {
  state.value = 'connecting'
  ring = []
  nextSeq = 0
  ready = false
  stopRequested = false
  attempt = 0
  resumeToken = ''
  recorder = new Recorder({ onFrame, onLevel, onSourceEnded })
  try {
    await recorder.start({ mic: micStream, sys: sysStream })
  } catch (e) {
    fail(`无法启动音频处理：${e.message || e.name}`)
    return
  }
  if (unmounted) return releaseCapture()
  hasMic.value = !!micStream
  hasSys.value = !!sysStream
  connect()
}

function onLevel(l) {
  level.mic = l.mic
  level.sys = l.sys
}

function onSourceEnded(name) {
  if (name === 'sys') {
    hasSys.value = false
    level.sys = 0
    warn(hasMic.value ? '已停止共享系统声音，继续仅录制麦克风。'
      : '已停止共享系统声音，且未开启麦克风，后续不会再录到声音。请停止并保存。')
  } else {
    hasMic.value = false
    level.mic = 0
    warn(hasSys.value ? '麦克风已断开，继续仅录制系统声音。'
      : '麦克风已断开，后续不会再录到声音。请停止并保存。')
  }
}

function onFrame(pcm) {
  const buf = new ArrayBuffer(4 + pcm.byteLength)
  new DataView(buf).setUint32(0, nextSeq, true)
  new Int16Array(buf, 4).set(pcm)
  const f = { seq: nextSeq++, buf }
  frameCount.value = nextSeq
  ring.push(f)
  if (ring.length > RING_FRAMES) ring.shift()
  if (!ready) return
  send(f.buf)
  if (ws && isStalled(ws.bufferedAmount)) {
    // half-open connection (no close event): drop it and resume on a fresh one
    closeWs()
    onDisconnected()
  }
}

// A large backlog right after a resume replay is normal as long as it drains; only a
// socket that accepts nothing for STALL_MS (bufferedAmount growing by every frame) is dead.
function isStalled(buffered) {
  const draining = buffered < lastBuffered + FRAME_BYTES
  lastBuffered = buffered
  if (buffered <= STALL_BYTES || draining) {
    stallSince = 0
    return false
  }
  if (!stallSince) stallSince = Date.now()
  return Date.now() - stallSince > STALL_MS
}

// ---- websocket ---------------------------------------------------------------

function wsUrl() {
  return `${location.protocol === 'https:' ? 'wss:' : 'ws:'}//${location.host}/web/ws`
}

function send(data) {
  if (!ws || ws.readyState !== WebSocket.OPEN) return false
  ws.send(data)
  return true
}

function sendJson(obj) {
  return send(JSON.stringify(obj))
}

function closeWs() {
  clearTimeout(retryTimer)
  retryTimer = null
  ready = false
  stallSince = lastBuffered = 0
  if (ws) {
    ws.onopen = ws.onmessage = ws.onclose = ws.onerror = null
    try {
      ws.close()
    } catch {
      /* ignore */
    }
    ws = null
  }
}

function connect() {
  closeWs()
  const sock = new WebSocket(wsUrl())
  sock.binaryType = 'arraybuffer'
  ws = sock
  sock.onopen = () => {
    if (recId.value) {
      sendJson({ type: 'resume', recording_id: recId.value, resume_token: resumeToken,
                 next_seq: ring.length ? ring[0].seq : nextSeq })
    } else {
      sendJson({ type: 'start', codec: 'pcm_s16le', sample_rate: 16000, ch: 1, client: CLIENT })
    }
  }
  sock.onmessage = (e) => {
    if (typeof e.data !== 'string') return
    let m
    try {
      m = JSON.parse(e.data)
    } catch {
      return
    }
    onMessage(m)
  }
  sock.onclose = () => {
    if (ws !== sock) return
    ws = null
    ready = false
    onDisconnected()
  }
}

function onDisconnected() {
  if (!recId.value) {
    // never got a recording id: most likely the reverse proxy blocks WebSocket
    fail('无法连接录音服务。若通过群晖反向代理访问，请检查反代规则是否添加了 WebSocket 自定义标头。')
    return
  }
  if (!['recording', 'reconnecting', 'stopping'].includes(state.value)) return
  if (state.value === 'recording') state.value = 'reconnecting'
  const delay = BACKOFF_MS[Math.min(attempt, BACKOFF_MS.length - 1)]
  attempt++
  clearTimeout(retryTimer)
  retryTimer = setTimeout(connect, delay)
}

function onMessage(m) {
  switch (m.type) {
    case 'started': {
      ready = true
      attempt = 0
      let from = 0
      if (m.resumed) {
        from = m.next_seq
      } else {
        recId.value = m.recording_id
        resumeToken = m.resume_token
      }
      for (const f of ring) if (f.seq >= from) send(f.buf)
      if (stopRequested) sendJson({ type: 'stop' })
      else state.value = 'recording'
      break
    }
    case 'partial':
      partial.value = m.text
      break
    case 'final':
      if (!finals.value.some((s) => s.idx === m.idx)) finals.value.push(m)
      partial.value = ''
      scrollDown()
      break
    case 'saved':
      ready = false
      releaseCapture()
      closeWs()
      recId.value = m.recording_id
      state.value = stopRequested ? 'saved' : 'interrupted'
      break
    case 'error':
      onServerError(m)
      break
    default:
      break
  }
}

function onServerError(m) {
  if (m.code === 'asr_error') {
    warn(`语音识别出错（${m.msg}），音频仍在录制并会保存，可稍后回放。`)
  } else if (m.code === 'resume_failed') {
    releaseCapture()
    closeWs()
    state.value = 'interrupted'
    warn('续录失败：服务端已不再接受这条录音（可能已重启）。')
  } else if (!recId.value) {
    fail(`服务端拒绝录音：${m.msg || m.code}`)
  } else {
    warn(`服务端错误：${m.msg || m.code}`)
  }
}

async function stop() {
  if (!busy.value || stopRequested) return
  stopRequested = true
  state.value = 'stopping'
  if (recorder) await recorder.flush()
  releaseCapture()
  // if not connected right now, the stop is sent after the resume succeeds
  if (ready) sendJson({ type: 'stop' })
  else if (!recId.value) {
    closeWs()
    state.value = 'idle'
  }
}

function scrollDown() {
  nextTick(() => {
    const el = list.value
    if (el) el.scrollTop = el.scrollHeight
  })
}

// ---- page lifecycle ----------------------------------------------------------

function onBeforeUnload(e) {
  if (!busy.value) return
  e.preventDefault()
  e.returnValue = ''
}

onBeforeRouteLeave(() => {
  if (!busy.value) return true
  return confirm('正在录音，离开本页会中断录音（已录部分会保存）。确定离开？')
})

watch(useMic, (v) => {
  if (v && !mics.value.length) refreshMics()
})

onMounted(() => {
  window.addEventListener('beforeunload', onBeforeUnload)
  navigator.mediaDevices?.addEventListener?.('devicechange', refreshMics)
  refreshMics()
})

onBeforeUnmount(() => {
  unmounted = true
  window.removeEventListener('beforeunload', onBeforeUnload)
  navigator.mediaDevices?.removeEventListener?.('devicechange', refreshMics)
  releaseCapture()
  closeWs()
})
</script>

<style scoped>
.warn { background: #fffbeb; border-color: #fcd34d; color: #92400e; }
.prompt { margin: 12px 0 0; }
.opt { display: inline-flex; align-items: center; gap: 4px; cursor: pointer; }
.mic-select { max-width: 280px; }
.controls { margin-top: 16px; gap: 20px; }
button.big { font-size: 16px; padding: 8px 20px; }
.duration { font-family: monospace; font-size: 24px; min-width: 90px; }
.levels { display: flex; flex-direction: column; gap: 6px; }
.level-row { display: flex; align-items: center; gap: 8px; }
.level-row .muted { width: 56px; }
.level-row.off { opacity: 0.4; }
.meter { width: 180px; height: 8px; background: #e5e7eb; border-radius: 4px; overflow: hidden; display: inline-block; }
.meter span { display: block; height: 100%; background: #16a34a; transition: width 0.08s linear; }
.warn-line { color: #92400e; margin: 8px 0 0; }
.ok { color: #166534; }
.transcript { max-height: 420px; overflow-y: auto; }
.sentence { padding: 6px 4px; border-bottom: 1px solid #f3f4f6; line-height: 1.6; }
.sentence.partial { color: #6b7280; font-style: italic; }
.spk { font-weight: 600; margin-right: 8px; }
.time { font-family: monospace; }
</style>
