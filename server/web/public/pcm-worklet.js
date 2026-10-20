// AudioWorklet processor for browser recording (served as /pcm-worklet.js, loaded by
// src/recorder.js). Two inputs: 0 = microphone, 1 = system audio, each already
// down-mixed to mono by the node (channelCount 1, explicit). The processor mixes them,
// low-passes and resamples from the context rate to 16 kHz, converts to s16le and
// posts 3200-sample (200 ms) frames plus per-input peak levels.
//
// Messages to main thread:
//   { type: 'frame', pcm: ArrayBuffer }        Int16Array(3200) little-endian
//   { type: 'level', mic: 0..1, sys: 0..1 }   about every 100 ms
//   { type: 'flushed' }                        reply to 'flush' (last frame padded)

const OUT_RATE = 16000
const FRAME_SAMPLES = 3200
const TAPS = 31

// Windowed-sinc low-pass (Blackman), cutoff in cycles/sample of the input rate.
function lowpass(cutoff) {
  const h = new Float32Array(TAPS)
  const m = (TAPS - 1) / 2
  let sum = 0
  for (let i = 0; i < TAPS; i++) {
    const x = i - m
    const sinc = x === 0 ? 2 * cutoff : Math.sin(2 * Math.PI * cutoff * x) / (Math.PI * x)
    const w = 0.42 - 0.5 * Math.cos((2 * Math.PI * i) / (TAPS - 1)) +
      0.08 * Math.cos((4 * Math.PI * i) / (TAPS - 1))
    h[i] = sinc * w
    sum += h[i]
  }
  for (let i = 0; i < TAPS; i++) h[i] /= sum
  return h
}

class PcmWorklet extends AudioWorkletProcessor {
  constructor() {
    super()
    this.step = sampleRate / OUT_RATE // input samples per output sample
    this.taps = this.step > 1 ? lowpass((0.45 * OUT_RATE) / sampleRate) : null
    this.hist = new Float32Array(TAPS) // ring of recent mixed input samples
    this.histPos = 0
    this.prev = 0 // previous filtered sample
    this.t = 0 // position of next output sample between prev (0) and current (1)
    this.out = new Int16Array(FRAME_SAMPLES)
    this.n = 0
    this.peak = [0, 0]
    this.levelLeft = Math.round(sampleRate / 10)
    this.port.onmessage = (e) => {
      if (e.data === 'flush') {
        if (this.n > 0) {
          this.out.fill(0, this.n)
          this.emitFrame()
        }
        this.port.postMessage({ type: 'flushed' })
      }
    }
  }

  emitFrame() {
    const buf = this.out.buffer
    this.port.postMessage({ type: 'frame', pcm: buf }, [buf])
    this.out = new Int16Array(FRAME_SAMPLES)
    this.n = 0
  }

  filter(x) {
    if (!this.taps) return x
    const h = this.hist
    h[this.histPos] = x
    this.histPos = (this.histPos + 1) % TAPS
    let acc = 0
    let j = this.histPos
    for (let i = 0; i < TAPS; i++) {
      acc += this.taps[i] * h[j]
      j = j + 1 === TAPS ? 0 : j + 1
    }
    return acc
  }

  push(y) {
    const v = y > 1 ? 1 : y < -1 ? -1 : y
    this.out[this.n++] = v < 0 ? v * 0x8000 : v * 0x7fff
    if (this.n === FRAME_SAMPLES) this.emitFrame()
  }

  process(inputs) {
    const mic = inputs[0] && inputs[0][0]
    const sys = inputs[1] && inputs[1][0]
    const len = (mic || sys || []).length
    for (let i = 0; i < len; i++) {
      const a = mic ? mic[i] : 0
      const b = sys ? sys[i] : 0
      const pa = Math.abs(a)
      const pb = Math.abs(b)
      if (pa > this.peak[0]) this.peak[0] = pa
      if (pb > this.peak[1]) this.peak[1] = pb
      const x = this.filter(a + b)
      // linear interpolation between prev and x at the output sample positions
      while (this.t <= 1) {
        this.push(this.prev + (x - this.prev) * this.t)
        this.t += this.step
      }
      this.t -= 1
      this.prev = x
    }
    this.levelLeft -= len
    if (this.levelLeft <= 0) {
      this.port.postMessage({ type: 'level', mic: Math.min(1, this.peak[0]), sys: Math.min(1, this.peak[1]) })
      this.peak = [0, 0]
      this.levelLeft = Math.round(sampleRate / 10)
    }
    return true
  }
}

registerProcessor('pcm-worklet', PcmWorklet)
