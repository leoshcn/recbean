// Browser audio capture for /record: microphone (getUserMedia) + system audio
// (getDisplayMedia, "entire screen" + "share system audio"), mixed with Web Audio and
// turned into 16 kHz mono s16le frames of 3200 samples (200 ms) by public/pcm-worklet.js.
// Framework-free; Record.vue owns the WebSocket, sequence numbers and UI state.

export const FRAME_SAMPLES = 3200
export const FRAME_MS = 200

export function isSecure() {
  return window.isSecureContext === true
}

export function canCaptureMic() {
  return !!navigator.mediaDevices?.getUserMedia
}

export function canCaptureSystem() {
  return !!navigator.mediaDevices?.getDisplayMedia
}

// audioinput devices; labels are empty until microphone permission was granted once
export async function listMics() {
  if (!navigator.mediaDevices?.enumerateDevices) return []
  const all = await navigator.mediaDevices.enumerateDevices()
  return all
    .filter((d) => d.kind === 'audioinput' && d.deviceId !== 'default' && d.deviceId !== 'communications')
    .map((d, i) => ({ id: d.deviceId, label: d.label || `麦克风 ${i + 1}` }))
}

export function openMic(deviceId) {
  return navigator.mediaDevices.getUserMedia({
    audio: {
      deviceId: deviceId ? { exact: deviceId } : undefined,
      channelCount: 1,
      echoCancellation: true,
      noiseSuppression: true,
      autoGainControl: true,
    },
  })
}

// Ask the user to share a screen with system audio. Returns the stream with only its
// audio track(s) left running, or null when the user shared without audio.
export async function openSystemAudio() {
  const stream = await navigator.mediaDevices.getDisplayMedia({
    video: true, // required by Chrome/Edge; stopped right away
    audio: { echoCancellation: false, noiseSuppression: false, autoGainControl: false },
    systemAudio: 'include',
    selfBrowserSurface: 'exclude',
    surfaceSwitching: 'exclude',
    monitorTypeSurfaces: 'include',
  })
  for (const t of stream.getVideoTracks()) {
    t.stop()
    stream.removeTrack(t)
  }
  if (!stream.getAudioTracks().length) return null
  return stream
}

export function stopStream(stream) {
  if (stream) for (const t of stream.getTracks()) t.stop()
}

// Mixes the given streams and emits frames / levels until stop().
//   onFrame(Int16Array)          every 200 ms of audio
//   onLevel({ mic, sys })        peak 0..1 per source, ~10 per second
//   onSourceEnded('mic'|'sys')   a source's track ended (e.g. browser "停止共享")
export class Recorder {
  constructor({ onFrame, onLevel, onSourceEnded }) {
    this.onFrame = onFrame
    this.onLevel = onLevel
    this.onSourceEnded = onSourceEnded
    this.ctx = null
    this.node = null
    this.sources = {} // name -> { stream, src, gain }
    this._flushed = null
  }

  async start({ mic, sys }) {
    const ctx = new AudioContext({ latencyHint: 'interactive' })
    this.ctx = ctx
    await ctx.audioWorklet.addModule(`${import.meta.env.BASE_URL}pcm-worklet.js`)
    const node = new AudioWorkletNode(ctx, 'pcm-worklet', {
      numberOfInputs: 2,
      numberOfOutputs: 1,
      outputChannelCount: [1],
      channelCount: 1,
      channelCountMode: 'explicit',
      channelInterpretation: 'speakers',
    })
    this.node = node
    node.port.onmessage = (e) => {
      const m = e.data
      if (m.type === 'frame') this.onFrame?.(new Int16Array(m.pcm))
      else if (m.type === 'level') this.onLevel?.({ mic: m.mic, sys: m.sys })
      else if (m.type === 'flushed' && this._flushed) this._flushed()
    }
    // The worklet writes no output (silence); connecting it keeps it in the render graph.
    node.connect(ctx.destination)
    if (mic) this._add('mic', mic, 0)
    if (sys) this._add('sys', sys, 1)
    if (ctx.state === 'suspended') await ctx.resume()
  }

  _add(name, stream, input) {
    const src = this.ctx.createMediaStreamSource(stream)
    const gain = this.ctx.createGain()
    src.connect(gain)
    gain.connect(this.node, 0, input)
    this.sources[name] = { stream, src, gain }
    for (const t of stream.getAudioTracks()) {
      t.addEventListener('ended', () => {
        if (this.sources[name]?.stream !== stream) return
        this.removeSource(name)
        this.onSourceEnded?.(name)
      })
    }
  }

  hasSource(name) {
    return !!this.sources[name]
  }

  removeSource(name) {
    const s = this.sources[name]
    if (!s) return
    delete this.sources[name]
    try {
      s.src.disconnect()
      s.gain.disconnect()
    } catch {
      /* already disconnected */
    }
    stopStream(s.stream)
  }

  // Emit the last partial frame (zero padded); resolves once it was posted.
  flush(timeoutMs = 1000) {
    if (!this.node) return Promise.resolve()
    return new Promise((resolve) => {
      const timer = setTimeout(resolve, timeoutMs)
      this._flushed = () => {
        clearTimeout(timer)
        this._flushed = null
        resolve()
      }
      this.node.port.postMessage('flush')
    })
  }

  stop() {
    for (const name of Object.keys(this.sources)) this.removeSource(name)
    if (this.node) {
      this.node.port.onmessage = null
      this.node.disconnect()
      this.node = null
    }
    if (this.ctx) {
      this.ctx.close().catch(() => {})
      this.ctx = null
    }
  }
}
