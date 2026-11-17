// Bridge to the bundled helper programs (Tauri sidecars): the C++ audio engine (`sermon_clean`) and
// the file converter (`ffmpeg`). The engine prints one JSON object per line; the Rust side forwards
// every line of output as an `engine-event`. See docs/app-architecture.md for the protocol.
import { invoke } from '@tauri-apps/api/core'
import { listen, type UnlistenFn } from '@tauri-apps/api/event'

export interface EngineEvent {
  event: string
  [key: string]: unknown
}

interface Payload {
  job_id: string
  kind: 'line' | 'stderr' | 'exit'
  data: string
}

export interface Progress {
  label: string
  step: number
  total: number
}

let counter = 0

interface RawJob {
  /** Resolves with the exit code once the program has finished. */
  exit: Promise<number>
  cancel: () => void
  readonly cancelled: () => boolean
}

/** Runs a bundled program, calling back for every output line and stderr chunk. */
function startJob(sidecar: 'sermon_clean' | 'ffmpeg', args: string[], onLine: (line: string) => void, onStderr?: (text: string) => void): RawJob {
  const jobId = `job-${Date.now()}-${counter++}`
  let unlisten: UnlistenFn | null = null
  let cancelled = false

  const exit = new Promise<number>((resolve, reject) => {
    listen<Payload>('engine-event', (e) => {
      const p = e.payload
      if (p.job_id !== jobId) return
      if (p.kind === 'line') onLine(p.data)
      else if (p.kind === 'stderr') onStderr?.(p.data)
      else {
        unlisten?.()
        resolve(Number(p.data))
      }
    })
      .then((fn) => {
        unlisten = fn
        return invoke('run_sidecar', { jobId, sidecar, args })
      })
      .catch((err) => {
        unlisten?.()
        reject(new Error(typeof err === 'string' ? err : err instanceof Error ? err.message : 'Could not start the audio engine.'))
      })
  })

  return {
    exit,
    cancel: () => {
      cancelled = true
      void invoke('cancel_engine', { jobId })
    },
    cancelled: () => cancelled,
  }
}

export interface EngineJob {
  /** Resolves with the engine's final event (`probe`, `analysis` or `result`). */
  done: Promise<EngineEvent>
  cancel: () => void
}

export function runEngine(args: string[], onProgress?: (p: Progress) => void): EngineJob {
  let final: EngineEvent | null = null
  let errorMessage: string | null = null
  let stderr = ''
  const job = startJob(
    'sermon_clean',
    args,
    (line) => {
      let ev: EngineEvent
      try {
        ev = JSON.parse(line) as EngineEvent
      } catch {
        return // not one of ours; ignore stray text
      }
      if (ev.event === 'progress') onProgress?.({ label: String(ev.label ?? ''), step: Number(ev.step ?? 0), total: Number(ev.total ?? 0) })
      else if (ev.event === 'error') errorMessage = String(ev.message ?? 'The audio engine reported an error.')
      else final = ev
    },
    (text) => {
      stderr += text
    },
  )
  const done = job.exit.then((code) => {
    if (job.cancelled()) throw new Error('Cancelled')
    if (errorMessage) throw new Error(errorMessage)
    if (code !== 0 || !final) throw new Error(stderr.trim() || `The audio engine stopped unexpectedly (code ${code}).`)
    return final
  })
  return { done, cancel: job.cancel }
}

export interface RawResult {
  code: number
  text: string // stdout and stderr together
}

export interface RawRun {
  done: Promise<RawResult>
  cancel: () => void
}

/** Runs ffmpeg: reports each progress line and resolves with everything it printed. */
export function runFfmpeg(args: string[], onLine?: (line: string) => void): RawRun {
  let text = ''
  const job = startJob(
    'ffmpeg',
    args,
    (line) => {
      text += line + '\n'
      onLine?.(line)
    },
    (t) => {
      text += t
    },
  )
  const done = job.exit.then((code) => {
    if (job.cancelled()) throw new Error('Cancelled')
    return { code, text }
  })
  return { done, cancel: job.cancel }
}

export const scratchPath = (name: string) => invoke<string>('scratch_path', { name })
export const logLine = (msg: string) => {
  void invoke('log_line', { msg }).catch(() => {})
}
export const readDiagnostics = () => invoke<string>('read_diagnostics')
export const openReleasePage = (url: string) => invoke<void>('open_release_page', { url })
export const clearScratch = () => invoke<void>('clear_scratch')
export const removeScratchFile = (path: string) => invoke<void>('remove_scratch_file', { path })
