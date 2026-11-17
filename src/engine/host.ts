// Client for the real-time engine (`sermon_host` sidecar): one long-lived process that plays the
// recording through the live chain. Commands go in as JSON lines (Rust `write_sidecar`), state and
// meters come back as `engine-event` lines.
import { invoke } from '@tauri-apps/api/core'
import { listen, type UnlistenFn } from '@tauri-apps/api/event'
import type { Settings } from './settings'

export interface HostState {
  position: number
  playing: boolean
  applying: boolean // a settings change is being rendered and isn't audible yet
  atEnd: boolean
  peakDb: number
  momentary: number
  shortTerm: number
  leveler: number // dB of gain the leveler is applying right now
  comp: number // dB of gain reduction (positive)
  deess: number
  limiter: number
  underruns: number
  load: number
}

interface Payload {
  job_id: string
  kind: 'line' | 'stderr' | 'exit'
  data: string
}

export interface HostHandlers {
  onState: (s: HostState) => void
  onProgress: (label: string, fraction: number) => void
  onRender?: (ev: { kind: 'progress' | 'done' | 'error'; label?: string; fraction?: number; seconds?: number; lufs?: number; message?: string }) => void
  onBake?: (state: 'off' | 'working' | 'ready' | 'failed') => void
  onLoudness?: (gainDb: number, lufs: number) => void
  onError: (message: string) => void
  onExit: () => void
}

export interface HostConnection {
  send: (cmd: Record<string, unknown>) => void
  /** Resolves with the duration once the recording is open and playable. */
  opened: Promise<number>
  close: () => void
}

let counter = 0

export function startHost(h: HostHandlers, open: { path: string; work: string; peaks: string }): HostConnection {
  const jobId = `host-${Date.now()}-${counter++}`
  let unlisten: UnlistenFn | null = null
  let ready = false
  let closed = false
  const queue: string[] = []
  let resolveOpened!: (d: number) => void
  let rejectOpened!: (e: Error) => void
  const opened = new Promise<number>((res, rej) => {
    resolveOpened = res
    rejectOpened = rej
  })

  const write = (line: string) => {
    if (!ready) queue.push(line)
    else void invoke('write_sidecar', { jobId, line }).catch(() => {})
  }

  listen<Payload>('engine-event', (e) => {
    const p = e.payload
    if (p.job_id !== jobId) return
    if (p.kind === 'line') {
      let ev: Record<string, unknown>
      try {
        ev = JSON.parse(p.data)
      } catch {
        return
      }
      if (ev.event === 'state') h.onState(ev as unknown as HostState)
      else if (ev.event === 'progress') h.onProgress(String(ev.label ?? ''), Number(ev.fraction ?? 0))
      else if (ev.event === 'render') h.onRender?.({ kind: 'progress', label: String(ev.label ?? ''), fraction: Number(ev.fraction ?? 0) })
      else if (ev.event === 'rendered') h.onRender?.({ kind: 'done', seconds: Number(ev.seconds), lufs: Number(ev.lufs) })
      else if (ev.event === 'render_error') h.onRender?.({ kind: 'error', message: String(ev.message ?? 'The export failed.') })
      else if (ev.event === 'bake') h.onBake?.(ev.state as 'off' | 'working' | 'ready' | 'failed')
      else if (ev.event === 'loudness') h.onLoudness?.(Number(ev.gainDb), Number(ev.lufs))
      else if (ev.event === 'opened') resolveOpened(Number(ev.duration))
      else if (ev.event === 'error') {
        rejectOpened(new Error(String(ev.message)))
        h.onError(String(ev.message ?? 'The audio engine reported an error.'))
      }
    } else if (p.kind === 'exit') {
      unlisten?.()
      if (!closed) {
        rejectOpened(new Error('The audio engine stopped unexpectedly.'))
        h.onExit()
      }
    }
  })
    .then((fn) => {
      unlisten = fn
      return invoke('run_sidecar', { jobId, sidecar: 'sermon_host', args: [] })
    })
    .then(() => {
      ready = true
      void invoke('write_sidecar', { jobId, line: JSON.stringify({ cmd: 'open', ...open }) })
      for (const l of queue.splice(0)) void invoke('write_sidecar', { jobId, line: l })
    })
    .catch((err) => rejectOpened(new Error(typeof err === 'string' ? err : 'Could not start the audio engine.')))
  opened.catch(() => {})

  return {
    send: (cmd) => write(JSON.stringify(cmd)),
    opened,
    close: () => {
      closed = true
      if (ready) void invoke('write_sidecar', { jobId, line: '{"cmd":"shutdown"}' }).catch(() => {})
      setTimeout(() => void invoke('cancel_engine', { jobId }).catch(() => {}), 1500)
      unlisten?.()
    },
  }
}

/** The live-chain parameters for the UI's settings. Declip and the dereverb correction are prepared by the engine in the background (see `bake`
 *  events); the dereverb amount itself is then live; the engine solves the output gain itself (the −14 LUFS target) and reports it as a `loudness` event. */
export function liveParams(s: Settings): Record<string, unknown> {
  return {
    cmd: 'params',
    levelAmt: s.leveling,
    declip: s.declip,
    dereverb: s.dereverb,
    noise: s.noise,
    toneAmt: s.tone,
    deess: s.deess,
    comp: s.compressor,
    loudAmt: s.loudness,
    pregain: s.pregain,
  }
}
