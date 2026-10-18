async function request(method, url, body) {
  const opts = { method, headers: {} }
  if (body !== undefined) {
    opts.headers['Content-Type'] = 'application/json'
    opts.body = JSON.stringify(body)
  }
  const res = await fetch(url, opts)
  if (!res.ok) {
    let msg = `${res.status}`
    try {
      const data = await res.json()
      if (data.detail) msg = typeof data.detail === 'string' ? data.detail : JSON.stringify(data.detail)
    } catch {
      /* ignore */
    }
    throw new Error(msg)
  }
  return res.json()
}

const enc = encodeURIComponent

export const api = {
  listRecordings: () => request('GET', '/api/recordings'),
  getRecording: (id) => request('GET', `/api/recordings/${enc(id)}`),
  updateTitle: (id, title) => request('PATCH', `/api/recordings/${enc(id)}`, { title }),
  deleteRecording: (id) => request('DELETE', `/api/recordings/${enc(id)}`),
  updateSentence: (id, sid, patch) =>
    request('PATCH', `/api/recordings/${enc(id)}/sentences/${sid}`, patch),
  renameSpeaker: (id, idx, name) =>
    request('PATCH', `/api/recordings/${enc(id)}/speakers/${idx}`, { name }),
  mergeSpeakers: (id, from, into) =>
    request('POST', `/api/recordings/${enc(id)}/speakers/merge`, { from, into }),
  audioUrl: (id) => `/api/recordings/${enc(id)}/audio`,
  exportUrl: (id) => `/api/recordings/${enc(id)}/export.txt`,
  listPairings: () => request('GET', '/api/pairings'),
  confirmPairing: (code, name) => request('POST', '/api/pairings', { code, name: name || null }),
  listDevices: () => request('GET', '/api/devices'),
  deleteDevice: (id) => request('DELETE', `/api/devices/${enc(id)}`),
}
