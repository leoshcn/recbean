<template>
  <div>
    <header class="page-head">
      <div class="titles">
        <p class="eyebrow">Pairing</p>
        <h1>设备配对</h1>
        <p class="page-sub">设备联网后屏幕会显示 4 位配对码。核对后点击“确认”，设备即可开始使用。</p>
      </div>
    </header>

    <div class="card">
      <p class="section-label">待配对<span v-if="pairings.length" class="mono"> · {{ pairings.length }}</span></p>
      <div v-if="!pairings.length" class="muted waiting">
        <span class="pulse" aria-hidden="true" />暂无待配对设备。
      </div>
      <div v-for="p in pairings" :key="p.device_id" class="row pending">
        <span class="code mono" :aria-label="`配对码 ${p.code}`">
          <span v-for="(c, i) in String(p.code)" :key="i" class="digit">{{ c }}</span>
        </span>
        <span class="who">
          <span class="mono">设备 {{ p.device_id }}</span>
          <span class="muted mono">{{ fmtTime(p.created_at) }}</span>
        </span>
        <span class="spacer" />
        <input v-model="names[p.device_id]" placeholder="设备名称（可选）" class="name-input" />
        <button class="primary" @click="confirmCode(p.code, names[p.device_id])">确认</button>
      </div>
      <div class="row manual">
        <input v-model="manual" maxlength="4" placeholder="手动输入配对码" class="mono manual-input"
               inputmode="numeric" />
        <button @click="confirmCode(manual)" :disabled="!/^\d{4}$/.test(manual)">确认配对码</button>
      </div>
      <p v-if="message" class="alert msg" :class="isError ? 'error' : 'ok'">{{ message }}</p>
    </div>

    <div class="card">
      <p class="section-label">已配对设备<span v-if="devices.length" class="mono"> · {{ devices.length }}</span></p>
      <div v-if="!devices.length" class="muted">暂无已配对设备。</div>
      <div v-for="d in devices" :key="d.id" class="row device">
        <span class="dev-dot" aria-hidden="true" />
        <span class="who">
          <strong>{{ d.name || d.id }}</strong>
          <span class="muted mono">{{ d.id }} · 配对于 {{ fmtTime(d.paired_at) }}</span>
        </span>
        <span class="spacer" />
        <button class="danger" @click="unpair(d)">解除配对</button>
      </div>
    </div>
  </div>
</template>

<script setup>
import { onMounted, onUnmounted, reactive, ref } from 'vue'
import { api } from '../api'
import { fmtTime } from '../util'

const pairings = ref([])
const devices = ref([])
const names = reactive({})
const manual = ref('')
const message = ref('')
const isError = ref(false)
let timer = null

async function load() {
  try {
    ;[pairings.value, devices.value] = await Promise.all([api.listPairings(), api.listDevices()])
  } catch (e) {
    message.value = `加载失败：${e.message}`
    isError.value = true
  }
}

async function confirmCode(code, name) {
  try {
    const r = await api.confirmPairing(code, name)
    message.value = `已确认设备 ${r.device_id}，设备将自动完成配对。`
    isError.value = false
    manual.value = ''
    await load()
  } catch (e) {
    message.value = `配对失败：${e.message}`
    isError.value = true
  }
}

async function unpair(d) {
  if (!confirm(`解除配对后设备 ${d.name || d.id} 需要重新配对，确定？`)) return
  await api.deleteDevice(d.id)
  await load()
}

onMounted(() => {
  load()
  timer = setInterval(load, 2000)
})
onUnmounted(() => clearInterval(timer))
</script>

<style scoped>
.pending, .device { padding: 12px 0; border-bottom: 1px solid var(--border); gap: 10px 14px; }
.device:last-child { border-bottom: none; }
.code { display: inline-flex; gap: 6px; }
.digit {
  display: inline-grid; place-items: center; width: 40px; height: 52px;
  font-size: 28px; font-weight: 650; color: var(--accent-strong);
  background: var(--accent-soft); border: 1px solid color-mix(in oklch, var(--accent) 30%, transparent);
  border-radius: var(--radius);
}
.who { display: flex; flex-direction: column; min-width: 0; line-height: 1.4; overflow-wrap: anywhere; }
.who .muted { font-size: 12px; }
.name-input { width: 180px; }
.manual { margin-top: 14px; }
.manual-input { width: 160px; letter-spacing: 0.2em; }
.msg { margin: 14px 0 0; }
.waiting { display: flex; align-items: center; gap: 10px; padding: 6px 0; }
.pulse {
  width: 8px; height: 8px; border-radius: 50%; background: var(--accent);
  animation: pulse 1.6s ease-in-out infinite;
}
@keyframes pulse { 50% { opacity: .25; } }
.dev-dot { width: 8px; height: 8px; border-radius: 50%; background: var(--ok); flex: none; }
@media (max-width: 767px) {
  .name-input { width: 100%; }
  .pending .spacer { display: none; }
}
</style>
