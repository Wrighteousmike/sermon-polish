import { useCallback, useEffect, useRef, useState } from 'react'
import { invoke } from '@tauri-apps/api/core'
import { save } from '@tauri-apps/plugin-dialog'
import { runEngine, scratchPath, type Progress } from '../engine/engine'
import type { Settings } from '../engine/settings'
import { formatFileSize } from '../lib/time'
import { buildFilename, parseFilenameForTags, type TagFields } from '../lib/tagging'
import { etaText, RenderEta } from '../lib/eta'
import type { Source } from '../engine/source'

/** A render of the kept part, through the studio's own engine (what you heard is what gets exported). */
export interface Rendering {
  done: Promise<{ seconds: number }>
  cancel: () => void
}

export interface ExportRequest {
  settings: Settings
  trim: { start: number; end: number }
  render: (outPath: string, onProgress: (label: string, fraction: number) => void) => Rendering
  /** A cleaned file already made for exactly these settings and this trim: use it instead of rendering again. */
  cached?: string | null
  /** Called with the cleaned file once it exists, so the studio can keep it for next time. */
  onRendered?: (path: string) => void
}

interface ExportStepProps {
  source: Source
  request: ExportRequest
  onBack: () => void
}

function useEta() {
  const eta = useRef(new RenderEta(navigator.hardwareConcurrency || 8))
  const [text, setText] = useState('')
  const reset = useCallback(() => {
    eta.current = new RenderEta(navigator.hardwareConcurrency || 8)
    setText('')
  }, [])
  const apply = (seconds: number | null) => setText(seconds === null ? '' : etaText(seconds))
  /** Progress from the engine's render. */
  const updateRender = useCallback((label: string, fraction: number) => apply(eta.current.render(label, fraction, Date.now())), [])
  /** A simple steady job (encoding the MP3): estimate from the share done so far. */
  const updateSimple = useCallback((fraction: number) => apply(eta.current.simple(fraction, Date.now())), [])
  return { text, reset, updateRender, updateSimple }
}

const ARTIST = 'oneflock.co.za ministry tools'
const VBR_ESTIMATE_KBPS = 36 // measured on speech at 22.05 kHz mono, quality 8

// The export formats. `mp3Preset` ids are the engine's (native/src/mp3_export.cpp); the WAV is
// written by the engine's --wav-mono16.
interface Format {
  id: string
  label: string
  description: string
  ext: 'mp3' | 'wav'
  mp3Preset?: string
  bytesPerSecond: number
}
const FORMATS: Format[] = [
  { id: 'wav', label: 'WAV, 16-bit', description: 'Mono, 44.1kHz, 16-bit, uncompressed', ext: 'wav', bytesPerSecond: 44100 * 2 },
  { id: 'mp3-320', label: 'MP3, highest quality', description: 'Mono, 44.1kHz, 320kbps', ext: 'mp3', mp3Preset: 'mono-320', bytesPerSecond: (320 * 1000) / 8 },
  { id: 'mp3-192', label: 'MP3, standard', description: 'Mono, 44.1kHz, 192kbps', ext: 'mp3', mp3Preset: 'mono-192', bytesPerSecond: (192 * 1000) / 8 },
  { id: 'mp3-small', label: 'MP3, smallest file', description: 'Mono, 22.05kHz, variable bitrate (VBR 8)', ext: 'mp3', mp3Preset: 'mono-small', bytesPerSecond: (VBR_ESTIMATE_KBPS * 1000) / 8 },
]

const EMPTY_TAGS: TagFields = { date: '', text: '', title: '', speaker: '' }

export default function ExportStep({ source, request, onBack }: ExportStepProps) {
  const [tags, setTags] = useState<TagFields>(() => ({ ...EMPTY_TAGS, ...parseFilenameForTags(source.name) }))
  const [working, setWorking] = useState<string | null>(null)
  const [stage, setStage] = useState<'cleaning' | 'encoding' | null>(null)
  const [progress, setProgress] = useState<Progress | null>(null)
  const [saved, setSaved] = useState<string | null>(null)
  // The "export complete" pop-up stays until it is dismissed, so there is time to read it.
  const [complete, setComplete] = useState<{ path: string; format: string } | null>(null)
  const [revealError, setRevealError] = useState<string | null>(null)
  const [error, setError] = useState<string | null>(null)
  // The cleaned recording is rendered once and reused for every format saved from this screen.
  const renderedRef = useRef<string | null>(null)
  const renderPromiseRef = useRef<Promise<string> | null>(null)
  const [renderState, setRenderState] = useState<{ phase: 'waiting' | 'running' | 'done' | 'error'; label: string; fraction: number; message?: string }>({ phase: 'waiting', label: '', fraction: 0 })
  const eta = useEta()
  const jobRef = useRef<{ cancel: () => void } | null>(null)
  const seconds = request.trim.end - request.trim.start

  useEffect(
    () => () => {
      jobRef.current?.cancel()
      // The cleaned file belongs to the studio now (see ExportRequest.cached); it is cleared on Start again / quit.
    },
    [],
  )

  const setField = (key: keyof TagFields) => (e: React.ChangeEvent<HTMLInputElement>) => setTags((prev) => ({ ...prev, [key]: e.target.value }))
  const outputName = buildFilename(tags)

  // The cleaned recording is rendered once (it doesn't depend on the format) and reused for every format
  // saved from this screen. It starts as soon as this screen opens, while the details are filled in.
  const ensureRendered = (): Promise<string> => {
    if (renderPromiseRef.current) return renderPromiseRef.current
    if (request.cached) {
      renderedRef.current = request.cached
      setRenderState({ phase: 'done', label: '', fraction: 1 })
      renderPromiseRef.current = Promise.resolve(request.cached)
      return renderPromiseRef.current
    }
    setStage('cleaning')
    eta.reset()
    setRenderState({ phase: 'running', label: 'Cleaning the recording…', fraction: 0 })
    const promise = (async () => {
      const out = await scratchPath(`cleaned-${Date.now()}.wav`)
      const job = request.render(out, (label, fraction) => {
        setProgress({ label, step: Math.round(fraction * 100), total: 100 })
        setRenderState({ phase: 'running', label, fraction })
        eta.updateRender(label, fraction)
      })
      jobRef.current = job
      await job.done
      renderedRef.current = out
      request.onRendered?.(out)
      setRenderState({ phase: 'done', label: '', fraction: 1 })
      return out
    })()
    renderPromiseRef.current = promise
    promise.catch((err) => {
      renderPromiseRef.current = null  // a later Save tries again
      setRenderState({ phase: 'error', label: '', fraction: 0, message: err instanceof Error && err.message === 'Cancelled' ? 'Cancelled' : err instanceof Error ? err.message : 'The export failed.' })
    })
    return promise
  }

  useEffect(() => {
    // A moment's delay so a development double-mount doesn't send the render twice.
    const t = setTimeout(() => void ensureRendered().catch(() => {}), 150)
    return () => clearTimeout(t)
  }, []) // eslint-disable-line react-hooks/exhaustive-deps

  const handleSave = async (format: Format) => {
    setError(null)
    setSaved(null)
    const dest = await save({
      defaultPath: `${outputName}.${format.ext}`,
      filters: [{ name: format.ext === 'mp3' ? 'MP3 audio' : 'WAV audio', extensions: [format.ext] }],
    })
    if (!dest) return
    setWorking(format.id)
    setProgress(null)
    try {
      const cleaned = await ensureRendered()
      setStage('encoding')
      setProgress(null)
      eta.reset()
      const args = format.mp3Preset
        ? [cleaned, '-', '--no-chain', '--preset', format.mp3Preset, '--mp3-out', dest, '--title', outputName, '--artist', ARTIST, '--json']
        : [cleaned, '-', '--no-chain', '--wav-mono16', dest, '--json']
      const job = runEngine(args, (pr) => {
        setProgress(pr)
        if (pr.total) eta.updateSimple(pr.step / pr.total)
      })
      jobRef.current = job
      await job.done
      setSaved(dest)
      setRevealError(null)
      setComplete({ path: dest, format: format.label })
    } catch (err) {
      if (!(err instanceof Error && err.message === 'Cancelled')) setError(err instanceof Error ? err.message : 'Could not save this file.')
    } finally {
      jobRef.current = null
      setWorking(null)
      setStage(null)
    }
  }

  const showFile = async () => {
    if (!complete) return
    try {
      await invoke('reveal_in_folder', { path: complete.path })
      setRevealError(null)
    } catch (err) {
      setRevealError(typeof err === 'string' ? err : 'Could not open the folder.')
    }
  }
  useEffect(() => {
    if (!complete) return
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') setComplete(null)
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [complete])

  const busy = working !== null
  const status =
    stage === 'cleaning' ? `${progress?.label || 'Cleaning the recording…'}${progress && progress.total ? ` ${progress.step}%` : ''}` : progress?.label || 'Encoding…'

  return (
    <div className="download-step">
      {complete && (
        <div className="modal-backdrop" role="presentation">
          <div className="modal-card" role="alertdialog" aria-modal="true" aria-labelledby="export-done-title" aria-describedby="export-done-body">
            <h2 id="export-done-title">Export complete</h2>
            <p id="export-done-body">
              Your {complete.format.toLowerCase()} is saved:
              <br />
              <span className="mono export-done-path">{complete.path}</span>
            </p>
            {revealError && <p className="upload-error">{revealError}</p>}
            <div className="modal-actions">
              <button type="button" onClick={() => setComplete(null)}>Dismiss</button>
              <button type="button" className="primary" autoFocus onClick={() => void showFile()}>Show file</button>
            </div>
          </div>
        </div>
      )}
      <div className="export-head">
        <button type="button" disabled={busy} onClick={onBack}>← Back to studio</button>
        <p className="trim-hint">Exporting the part you kept ({Math.round(seconds / 60)} min) with the settings from the studio.</p>
      </div>

      <fieldset className="tag-fields">
        <legend>Sermon details</legend>
        <p className="trim-hint">Used to name the file and tag it, auto-filled where possible from the file you uploaded.</p>
        <div className="tag-grid">
          <label className="tag-field">
            <span>Date preached</span>
            <input type="text" placeholder="YYYY.MM.DD" value={tags.date} onChange={setField('date')} />
          </label>
          <label className="tag-field">
            <span>Text</span>
            <input type="text" placeholder="Scripture reference" value={tags.text} onChange={setField('text')} />
          </label>
          <label className="tag-field">
            <span>Title</span>
            <input type="text" placeholder="Sermon title" value={tags.title} onChange={setField('title')} />
          </label>
          <label className="tag-field">
            <span>Speaker</span>
            <input type="text" placeholder="Speaker name" value={tags.speaker} onChange={setField('speaker')} />
          </label>
        </div>
        <p className="tag-preview">
          Will save as: <span className="mono">{outputName}.mp3</span> or <span className="mono">.wav</span>
        </p>
      </fieldset>

      {renderState.phase === 'running' && (
        <div className="progress export-progress" role="status" aria-live="polite">
          <p className="applying measuring">
            <i className="spinner" aria-hidden /> <strong>{renderState.label || 'Cleaning the recording…'}</strong> {Math.round(renderState.fraction * 100)}%
            {renderState.label.startsWith('Preparing')
              ? <> · echo reduction is prepared first (no estimate yet)</>
              : eta.text ? <> · {eta.text}</> : <> · working out how long this will take…</>}
            <span className="export-progress-note"> · fill in the details meanwhile</span>
          </p>
          <div className="progress-bar"><div className="progress-fill" style={{ width: `${Math.max(3, renderState.fraction * 100)}%` }} /></div>
        </div>
      )}
      {renderState.phase === 'done' && <p className="trim-hint"><strong>Your recording is cleaned and ready.</strong> Choose a format to save; each one saves quickly.</p>}
      {renderState.phase === 'error' && renderState.message !== 'Cancelled' && <p className="upload-error">The export failed: {renderState.message}. Choosing a format will try again.</p>}
      {renderState.phase !== 'running' && renderState.phase !== 'done' && renderState.phase !== 'error' && <p className="trim-hint">Getting ready…</p>}

      {error && <p className="upload-error">{error}</p>}
      {saved && <p className="trim-hint">Saved to <span className="mono">{saved}</span></p>}

      <ul className="preset-list">
        {FORMATS.map((f) => (
          <li key={f.id} className="preset-row">
            <div className="preset-info">
              <p className="preset-label">{f.label}</p>
              <p className="preset-description">{f.description}</p>
              <p className="preset-size">about {formatFileSize(Math.ceil(seconds * f.bytesPerSecond))}</p>
            </div>
            <button type="button" className="primary" disabled={busy} onClick={() => void handleSave(f)}>
              {working === f.id ? `${status}${eta.text ? ` · ${eta.text}` : ''}` : 'Save…'}
            </button>
          </li>
        ))}
      </ul>

      {busy && (
        <button type="button" onClick={() => jobRef.current?.cancel()}>Cancel</button>
      )}
    </div>
  )
}
