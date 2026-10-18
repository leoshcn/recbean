<template>
  <div>
    <h2>设备配对</h2>
    <div class="card">
      <p class="muted" style="margin-top: 0">
        设备联网后屏幕会显示 4 位配对码。核对后点击“确认”，设备即可开始使用。
      </p>
      <div v-if="!pairings.length" class="muted">暂无待配对设备。</div>
      <div v-for="p in pairings" :key="p.device_id" class="row pending">
        <span class="code">{{ p.code }}</span>
        <span>设备 {{ p.device_id }}</span>
        <span class="muted">{{ fmtTime(p.created_at) }}</span>
        <span class="spacer" />
        <input v-model="names[p.device_id]" placeholder="设备名称（可选）" />
        <button class="primary" @click="confirmCode(p.code, names[p.device_id])">确认</button>
      </div>
      <div class="row" style="margin-top: 12px">
        <input v-model="manual" maxlength="4" placeholder="手动输入配对码" style="width: 140px" />
        <button @click="confirmCode(manual)" :disabled="!/^\d{4}$/.test(manual)">确认配对码</button>
      </div>
      <p v-if="message" :class="{ error: isError }">{{ message }}</p>
    </div>

    <h3>已配对设备</h3>
    <div class="card">
      <div v-if="!devices.length" class="muted">暂无已配对设备。</div>
      <div v-for="d in devices" :key="d.id" class="row device">
        <strong>{{ d.name || d.id }}</strong>
        <span class="muted">{{ d.id }}</span>
        <span class="muted">配对于 {{ fmtTime(d.paired_at) }}</span>
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
.pending, .device { padding: 8px 0; border-bottom: 1px solid #f3f4f6; }
.code { font-size: 24px; font-weight: 700; letter-spacing: 4px; font-family: monospace; }
</style>
