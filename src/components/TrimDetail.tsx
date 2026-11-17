import { useEffect, useRef, useState } from 'react'
import type { FinePeaks } from '../engine/source'
import { formatTimePrecise } from '../lib/time'

// Zoomed view around one trim marker, for setting the top and tail precisely. The window is
// centred on the marker; zoom changes how many seconds it spans. Click or drag in the waveform to
// place the marker, or nudge it with the buttons (or arrow keys: 0.1 s, Shift 1 s, Alt 0.01 s).
// The waveform is normalised to the loudest peak in view (switch off to see true level), so the start
// and end of a quiet recording are easy to see.

const WINDOWS = [120, 60, 30, 15, 8, 4, 2, 1, 0.5] // seconds across the view
const MIN_KEPT = 0.5 // the markers never get closer than this
const NORMALISE_FLOOR = 0.003 // about -50 dBFS: pure silence isn't blown up into noise

interface TrimDetailProps {
  label: string
  side: 'start' | 'end'
  time: number
  /** The other marker. */
  limit: number
  duration: number
  fine: FinePeaks
  disabled?: boolean
  onChange: (time: number) => void
  onHear: () => void
  hearDisabled?: boolean
  justReady?: boolean
  onUsePlayhead: () => void
}

export default function TrimDetail({ label, side, time, limit, duration, fine, disabled, onChange, onHear, hearDisabled, justReady, onUsePlayhead }: TrimDetailProps) {
  const canvasRef = useRef<HTMLCanvasElement>(null)
  const draggingRef = useRef(false)
  const [windowIdx, setWindowIdx] = useState(3) // 15 s to begin with
  const [center, setCenter] = useState(time)
  const [width, setWidth] = useState(600)
  const [normalise, setNormalise] = useState(() => {
    try {
      return localStorage.getItem('trim-normalise') !== 'off'
    } catch {
      return true
    }
  })
  const [viewGainDb, setViewGainDb] = useState(0)
  const windowSec = WINDOWS[windowIdx]

  const clamp = (t: number) => (side === 'start' ? Math.max(0, Math.min(t, limit - MIN_KEPT)) : Math.min(duration, Math.max(t, limit + MIN_KEPT)))
  const move = (t: number, recentre: boolean) => {
    const c = clamp(t)
    onChange(c)
    if (recentre) setCenter(c)
  }

  // Keep the window centred when the marker is moved from outside (the overview, the nudges).
  useEffect(() => {
    if (!draggingRef.current) setCenter(time)
  }, [time])

  useEffect(() => {
    const el = canvasRef.current
    if (!el) return
    const ro = new ResizeObserver(() => setWidth(Math.max(200, Math.floor(el.getBoundingClientRect().width))))
    ro.observe(el)
    return () => ro.disconnect()
  }, [])

  // Draw the waveform window, the part that will be cut, and the marker.
  useEffect(() => {
    const el = canvasRef.current
    if (!el) return
    const dpr = window.devicePixelRatio || 1
    const height = 110
    el.width = Math.floor(width * dpr)
    el.height = Math.floor(height * dpr)
    const ctx = el.getContext('2d')
    if (!ctx) return
    ctx.scale(dpr, dpr)
    const styles = getComputedStyle(el)
    const text = styles.getPropertyValue('--muted').trim() || '#666'
    const t0 = center - windowSec / 2
    const mid = height / 2
    const half = mid - 14

    ctx.clearRect(0, 0, width, height)
    const data = fine.data
    const bucket = fine.bucketSeconds
    // The loudest peak in view sets the scale, so quiet openings and endings fill the display.
    let scale = 1
    if (normalise) {
      let peak = 0
      const first = Math.max(0, Math.floor(t0 / bucket))
      const last = Math.min(Math.floor(data.length / 2), Math.ceil((t0 + windowSec) / bucket))
      for (let k = first; k < last; k++) peak = Math.max(peak, Math.abs(data[2 * k]), Math.abs(data[2 * k + 1]))
      scale = 1 / Math.max(peak / 32767, NORMALISE_FLOOR)
    }
    setViewGainDb(20 * Math.log10(scale))
    ctx.fillStyle = '#7d8fa0'
    for (let x = 0; x < width; x++) {
      const a = Math.floor((t0 + (x / width) * windowSec) / bucket)
      const b = Math.max(a + 1, Math.floor((t0 + ((x + 1) / width) * windowSec) / bucket))
      if (b <= 0 || a * 2 >= data.length) continue
      let mn = 32767
      let mx = -32768
      for (let k = Math.max(0, a); k < b && k * 2 + 1 < data.length; k++) {
        if (data[2 * k] < mn) mn = data[2 * k]
        if (data[2 * k + 1] > mx) mx = data[2 * k + 1]
      }
      if (mn > mx) continue
      const y1 = mid - Math.min(1, (mx / 32767) * scale) * half
      const y2 = mid - Math.max(-1, (mn / 32767) * scale) * half
      ctx.fillRect(x, y1, 1, Math.max(1, y2 - y1))
    }

    // what will be cut
    const xMarker = ((time - t0) / windowSec) * width
    ctx.fillStyle = 'rgba(59,71,86,0.35)'
    if (side === 'start') ctx.fillRect(0, 0, Math.max(0, Math.min(width, xMarker)), height)
    else ctx.fillRect(Math.max(0, Math.min(width, xMarker)), 0, width, height)

    // marker
    if (xMarker >= -2 && xMarker <= width + 2) {
      ctx.fillStyle = '#3d5a73'
      ctx.fillRect(Math.round(xMarker) - 1, 0, 2, height)
    }
    // window edge times
    ctx.fillStyle = text
    ctx.font = '11px system-ui, sans-serif'
    ctx.textBaseline = 'bottom'
    ctx.textAlign = 'left'
    ctx.fillText(formatTimePrecise(Math.max(0, t0)), 4, height - 2)
    ctx.textAlign = 'right'
    ctx.fillText(formatTimePrecise(Math.min(duration, t0 + windowSec)), width - 4, height - 2)
  }, [center, windowSec, time, width, fine, side, duration, normalise])

  const timeAt = (clientX: number) => {
    const rect = canvasRef.current!.getBoundingClientRect()
    return center - windowSec / 2 + ((clientX - rect.left) / rect.width) * windowSec
  }

  const nudge = (delta: number) => move(time + delta, true)

  return (
    <div className="trim-detail" data-tour={`fine-${side}`}>
      <div className="trim-detail-head">
        <strong>{label}</strong>
        <span className="mono">{formatTimePrecise(time)}</span>
        <label className="trim-detail-normalise" title="Scale the waveform to the loudest part in view, so quiet starts and ends are easy to see">
          <input
            type="checkbox"
            checked={normalise}
            onChange={(e) => {
              setNormalise(e.target.checked)
              try {
                localStorage.setItem('trim-normalise', e.target.checked ? 'on' : 'off')
              } catch {
                /* not remembered */
              }
            }}
          />{' '}
          Normalise{normalise && viewGainDb >= 1 ? ` (+${Math.round(viewGainDb)} dB)` : ''}
        </label>
        <span className="trim-detail-zoom">
          <button type="button" aria-label="Zoom out" title="Zoom out" disabled={windowIdx === 0} onClick={() => setWindowIdx((i) => Math.max(0, i - 1))}>
            −
          </button>
          <span>{windowSec >= 1 ? `${windowSec} s` : `${windowSec * 1000} ms`} view</span>
          <button type="button" aria-label="Zoom in" title="Zoom in" disabled={windowIdx === WINDOWS.length - 1} onClick={() => setWindowIdx((i) => Math.min(WINDOWS.length - 1, i + 1))}>
            +
          </button>
        </span>
      </div>
      <canvas
        ref={canvasRef}
        className="trim-detail-canvas"
        style={{ width: '100%', height: 110 }}
        tabIndex={0}
        role="slider"
        aria-label={`${label} marker position`}
        aria-valuemin={0}
        aria-valuemax={duration}
        aria-valuenow={time}
        aria-valuetext={formatTimePrecise(time)}
        onPointerDown={(e) => {
          if (disabled) return
          draggingRef.current = true
          e.currentTarget.setPointerCapture(e.pointerId)
          move(timeAt(e.clientX), false)
        }}
        onPointerMove={(e) => {
          if (draggingRef.current) move(timeAt(e.clientX), false)
        }}
        onPointerUp={(e) => {
          if (!draggingRef.current) return
          draggingRef.current = false
          e.currentTarget.releasePointerCapture(e.pointerId)
          setCenter(clamp(timeAt(e.clientX)))
        }}
        onKeyDown={(e) => {
          const step = e.shiftKey ? 1 : e.altKey ? 0.01 : 0.1
          if (e.key === 'ArrowLeft') {
            e.preventDefault()
            nudge(-step)
          } else if (e.key === 'ArrowRight') {
            e.preventDefault()
            nudge(step)
          }
        }}
      />
      <div className="trim-detail-buttons">
        {[-1, -0.1, 0.1, 1].map((d) => (
          <button key={d} type="button" disabled={disabled} onClick={() => nudge(d)}>
            {d > 0 ? '+' : '−'}
            {Math.abs(d)} s
          </button>
        ))}
        <button type="button" disabled={disabled} onClick={onUsePlayhead} title="Move this marker to where the main waveform is currently playing or was last clicked">
          Set to playhead
        </button>
        <button type="button" disabled={disabled} aria-disabled={hearDisabled || undefined} className={`${hearDisabled ? 'not-ready' : ''} ${justReady ? 'just-ready' : ''}`} title={hearDisabled ? 'Loudness processing' : undefined} onClick={onHear}>
          {side === 'start' ? 'Hear from here' : 'Hear up to here'}
        </button>
      </div>
    </div>
  )
}
