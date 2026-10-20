<template>
  <div class="record">
    <header class="page-head">
      <div class="titles">
        <p class="eyebrow">Browser recording</p>
        <h1>浏览器录音</h1>
        <p class="page-sub">录制麦克风与系统声音，实时转写，结束后保存为会议录音。</p>
      </div>
      <div class="actions">
        <span v-if="connText" class="badge" :class="connClass">{{ connText }}</span>
      </div>
    </header>

    <!-- narrow screens: recording is desktop-only; the console below is display:none there,
         so none of its controls can be reached -->
    <div class="card empty narrow-notice">
      <BrandMark :size="40" class="notice-mark" />
      请在桌面浏览器使用录音。
    </div>

    <div class="console-wrap">
      <div v-if="!secure" class="alert warn">
        请通过 HTTPS（群晖反代地址）访问本页面。浏览器只允许在安全连接下使用麦克风和屏幕共享，
        当前地址 {{ origin }} 无法录音。
      </div>

      <section class="card console">
        <div class="stage" :class="{ live: state === 'recording' }">
          <div class="stage-top mono">
            <span class="status"><span class="rec-dot" />{{ stageText }}</span>
            <span>16 kHz · MONO</span>
          </div>
          <BrandMark class="stage-mark" :size="110" :state="markState" :label="`录音状态：${stageText}`" />
          <div class="duration mono" aria-live="off">{{ fmtDuration(durationMs) }}</div>
          <div class="controls">
            <button v-if="!busy" class="rec-btn start" :disabled="!canStart" @click="start">
              <span class="disc" aria-hidden="true"><span class="glyph" /></span>
              <span class="cap">开始录音</span>
            </button>
            <button v-else class="rec-btn stop" :disabled="state === 'stopping' || state === 'acquiring' || state === 'needSys'"
                    @click="stop">
              <span class="disc" aria-hidden="true"><span class="glyph" /></span>
              <span class="cap">{{ state === 'stopping' ? '正在保存…' : '停止并保存' }}</span>
            </button>
          </div>
        </div>

        <div class="side">
          <p class="section-label">Sources · 音源</p>
          <div class="sources">
            <div class="src">
              <label class="opt">
                <input v-model="useMic" type="checkbox" :disabled="busy" /> 麦克风
              </label>
              <select v-model="micId" :disabled="busy || !useMic" class="mic-select">
                <option value="">默认麦克风</option>
                <option v-for="m in mics" :key="m.id" :value="m.id">{{ m.label }}</option>
              </select>
            </div>
            <div class="src">
              <label class="opt">
                <input v-model="useSys" type="checkbox" :disabled="busy || !sysSupported" /> 系统声音
              </label>
            </div>
          </div>
          <p v-if="secure && !sysSupported" class="muted hint">
            当前浏览器不支持共享系统声音，只能录制麦克风。请在 Windows 上使用 Chrome 或 Edge。
          </p>
          <p v-else-if="useSys && !busy" class="muted hint">
            开始后在弹窗中选择“整个屏幕”，并勾选“共享系统音频”。建议佩戴耳机，避免外放的声音被麦克风重复录入。
          </p>

          <p class="section-label levels-label">Levels · 电平</p>
          <div class="levels">
            <div class="level-row" :class="{ off: !hasMic }">
              <span class="muted">麦克风</span>
              <span class="progress segmented meter"><span :style="{ width: pct(level.mic) }" /></span>
            </div>
            <div class="level-row" :class="{ off: !hasSys }">
              <span class="muted">系统声音</span>
              <span class="progress segmented meter"><span :style="{ width: pct(level.sys) }" /></span>
            </div>
          </div>
        </div>
      </section>

      <div v-if="state === 'needSys'" class="alert warn prompt">
        <div>未获取到系统声音（取消了共享，或共享时没有选择“整个屏幕”并勾选“共享系统音频”）。</div>
        <div class="row" style="margin-top: 8px">
          <button class="primary" @click="pickSystem">重新选择共享</button>
          <button v-if="micStream" @click="continueMicOnly">仅录制麦克风</button>
          <button @click="cancelAcquire">取消</button>
        </div>
      </div>

      <p v-for="(w, i) in warnings" :key="i" class="alert warn">{{ w }}</p>
      <p v-if="error" class="alert error">{{ error }}</p>
      <p v-if="state === 'saved'" class="alert ok">
        已保存。<router-link :to="`/r/${encodeURIComponent(recId)}`">查看录音详情 →</router-link>
      </p>
      <p v-if="state === 'interrupted' && recId" class="alert error">
        录音已中断并保存。<router-link :to="`/r/${encodeURIComponent(recId)}`">查看录音详情 →</router-link>
      </p>

      <section class="card live-panel">
        <div class="row panel-head">
          <p class="section-label">Live transcript · 实时转写</p>
          <span class="spacer" />
          <span v-if="finals.length" class="muted mono">{{ finals.length }} 句</span>
        </div>
        <div ref="list" class="transcript">
          <div v-if="!finals.length && !partial" class="empty">{{ busy ? '等待语音…' : '开始录音后在这里显示转写' }}</div>
          <div v-for="s in finals" :key="s.idx" class="sentence">
            <div class="sentence-head">
              <span class="spk" :style="{ color: speakerColor(s.speaker) }">说话人{{ s.speaker }}</span>
              <span class="muted time">{{ fmtDuration(s.start_ms) }}</span>
            </div>
            <div>{{ s.text }}</div>
          </div>
          <div v-if="partial" class="sentence partial">{{ partial }}</div>
        </div>
      </section>
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
import BrandMark from '../components/BrandMark.vue'

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
// presentational only: console status line and logo state (recording = flowing slit,
// any transitional state = slit pulled straight, idle/saved/interrupted = standard mark)
const STAGE_TEXT = {
  idle: 'Ready', acquiring: '获取音源…', needSys: '等待共享', connecting: '连接中…', recording: 'REC',
  reconnecting: '重连中…', stopping: '保存中…', saved: '已保存', interrupted: '已中断',
}
const stageText = computed(() => STAGE_TEXT[state.value] || '')
const markState = computed(() => (state.value === 'recording' ? 'rec' : busy.value ? 'flat' : 'idle'))

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
.narrow-notice { display: none; }
.narrow-notice .notice-mark { margin: 0 auto 12px; }

/* ---- console ---- */
.console {
  display: grid; grid-template-columns: minmax(300px, 1fr) minmax(0, 1fr); gap: var(--gap-md);
  padding: 12px;
}
/* the stage is always the brand's dark field: Ink ground, Bean mark, Mist text */
.stage {
  position: relative; display: flex; flex-direction: column; align-items: center;
  padding: 18px 20px 22px; border-radius: 12px;
  background: var(--ink); color: var(--mist);
  border: 1px solid color-mix(in oklch, var(--mist) 8%, transparent);
}
.stage-top {
  align-self: stretch; display: flex; justify-content: space-between; gap: 12px;
  font-size: 11px; letter-spacing: 0.1em; text-transform: uppercase;
  color: color-mix(in oklch, var(--mist) 64%, var(--ink));
}
.status { display: inline-flex; align-items: center; gap: 8px; }
.rec-dot {
  width: 8px; height: 8px; border-radius: 50%;
  background: color-mix(in oklch, var(--mist) 40%, var(--ink));
}
.live .status { color: var(--mist); }
.live .rec-dot { background: var(--signal); animation: rec-blink 1.6s ease-in-out infinite; }
@keyframes rec-blink { 50% { opacity: .35; } }
.stage .stage-mark { color: var(--bean); margin: 22px 0 10px; }
.duration {
  font-size: 44px; font-weight: 500; line-height: 1.1; letter-spacing: -0.02em; color: var(--mist);
}
.controls { margin-top: 18px; }

/* big round record button: disc + caption */
.rec-btn {
  flex-direction: column; gap: 8px; min-height: 0; padding: 4px;
  border: none; background: transparent; color: var(--mist);
}
.rec-btn:hover:not(:disabled), .rec-btn:disabled:hover { background: transparent; border: none; }
.rec-btn:focus-visible { outline: none; }
.rec-btn:focus-visible .disc { outline: 2px solid var(--bean); outline-offset: 3px; }
.disc {
  display: grid; place-items: center; width: 76px; height: 76px; border-radius: 50%;
  transition: transform .15s, box-shadow .15s, background .15s;
}
.cap { font-size: 13px; color: color-mix(in oklch, var(--mist) 80%, var(--ink)); }
.start .disc { background: var(--bean); box-shadow: 0 0 0 6px color-mix(in oklch, var(--bean) 16%, transparent); }
.start .glyph { width: 24px; height: 24px; border-radius: 50%; background: var(--ink); }
.start:hover:not(:disabled) .disc { transform: scale(1.04); box-shadow: 0 0 0 9px color-mix(in oklch, var(--bean) 20%, transparent); }
.stop .disc { border: 2px solid var(--signal); background: color-mix(in oklch, var(--signal) 12%, transparent); }
.stop .glyph { width: 22px; height: 22px; border-radius: 4px; background: var(--signal); }
.stop:hover:not(:disabled) .disc { background: color-mix(in oklch, var(--signal) 22%, transparent); }
.rec-btn:disabled { opacity: .45; }

/* ---- side: sources + levels ---- */
.side { padding: 10px 8px 8px 0; min-width: 0; }
.sources { display: flex; flex-direction: column; gap: 10px; }
.src {
  display: flex; align-items: center; gap: 10px; flex-wrap: wrap;
  padding: 10px 12px; border: 1px solid var(--border); border-radius: var(--radius); background: var(--bg);
}
.opt { display: inline-flex; align-items: center; gap: 8px; cursor: pointer; font-size: 14px; font-weight: 500; }
.mic-select { flex: 1; min-width: 0; max-width: 100%; }
.hint { margin: 10px 0 0; line-height: 1.6; }
.levels-label { margin-top: var(--gap-md); }
.levels { display: flex; flex-direction: column; gap: 10px; }
.level-row { display: flex; align-items: center; gap: 10px; transition: opacity .15s; }
.level-row .muted { width: 60px; flex: none; }
.level-row.off { opacity: 0.4; }
.meter { flex: 1; height: 10px; }

/* ---- messages ---- */
.console-wrap > .alert { margin: 0 0 var(--gap-sm); }
.prompt .row { gap: 8px; }

/* ---- live transcript ---- */
.live-panel { margin-top: var(--gap-sm); }
.panel-head { margin-bottom: 4px; }
.panel-head .section-label { margin: 0; }
.transcript { max-height: 420px; overflow-y: auto; }
.sentence { padding: 8px 4px; border-bottom: 1px solid var(--border); line-height: 1.7; overflow-wrap: anywhere; }
.sentence:last-child { border-bottom: none; }
.sentence-head { display: flex; align-items: baseline; gap: 8px; }
.sentence.partial { color: var(--muted); font-style: italic; }
.sentence.partial::after {
  content: ''; display: inline-block; width: 7px; height: 1em; margin-left: 3px; vertical-align: -2px;
  background: var(--accent); animation: rec-blink 1s steps(2) infinite;
}
.spk { font-weight: 600; font-size: 13px; }
.time { font-family: var(--font-mono); font-variant-numeric: tabular-nums; font-size: 12px; }

@media (max-width: 900px) {
  .console { grid-template-columns: 1fr; }
  .side { padding: 4px 8px 8px; }
}
@media (max-width: 767px) {
  .console-wrap { display: none; }
  .narrow-notice { display: block; }
}
</style>
