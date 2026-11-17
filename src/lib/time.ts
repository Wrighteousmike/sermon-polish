export function formatTime(seconds: number): string {
  if (!Number.isFinite(seconds) || seconds < 0) return '0:00'
  const total = Math.floor(seconds)
  const h = Math.floor(total / 3600)
  const m = Math.floor((total % 3600) / 60)
  const s = total % 60
  return h > 0 ? `${h}:${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}` : `${m}:${String(s).padStart(2, '0')}`
}

/** m:ss.mmm, for placing trim markers precisely. */
export function formatTimePrecise(seconds: number): string {
  if (!Number.isFinite(seconds) || seconds < 0) return '0:00.000'
  const whole = Math.floor(seconds)
  const ms = Math.round((seconds - whole) * 1000)
  const carry = ms === 1000 ? 1 : 0
  const t = whole + carry
  const m = Math.floor(t / 60)
  return `${m}:${String(t % 60).padStart(2, '0')}.${String(carry ? 0 : ms).padStart(3, '0')}`
}

export function formatFileSize(bytes: number): string {
  const mb = bytes / (1024 * 1024)
  if (mb >= 1024) return `${(mb / 1024).toFixed(2)} GB`
  return `${mb.toFixed(1)} MB`
}
