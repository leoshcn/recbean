export function fmtDuration(ms) {
  const s = Math.max(0, Math.floor((ms || 0) / 1000))
  const h = Math.floor(s / 3600)
  const m = Math.floor((s % 3600) / 60)
  const sec = s % 60
  const pad = (n) => String(n).padStart(2, '0')
  return h ? `${h}:${pad(m)}:${pad(sec)}` : `${pad(m)}:${pad(sec)}`
}

export function fmtTime(epochMs) {
  const d = new Date(epochMs)
  const pad = (n) => String(n).padStart(2, '0')
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}`
}

// browser-local "YYYY-MM-DD" (same as the server's TZ_NAME day in the usual setup)
export function todayStr() {
  return fmtTime(Date.now()).slice(0, 10)
}

const WEEKDAYS =['周日', '周一', '周二', '周三', '周四', '周五', '周六']

// "2026-10-18" -> "2026-10-18 周日"（日期字符串由服务端按其时区给出，这里只算星期）
export function fmtDay(date) {
  const [y, m, d] = String(date).split('-').map(Number)
  const wd = new Date(Date.UTC(y, (m || 1) - 1, d || 1)).getUTCDay()
  return Number.isNaN(wd) ? date : `${date} ${WEEKDAYS[wd]}`
}

export const STATUS_TEXT = {
  recording: '录音中',
  done: '已完成',
  asr_error: '识别异常',
  interrupted: '已中断',
}

const COLORS = ['#2563eb', '#dc2626', '#16a34a', '#d97706', '#7c3aed', '#0891b2', '#db2777', '#4b5563']

export function speakerColor(idx) {
  return COLORS[(Math.max(1, idx) - 1) % COLORS.length]
}
