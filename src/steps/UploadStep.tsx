import { useEffect, useRef, useState } from 'react'
import { open } from '@tauri-apps/plugin-dialog'
import { getCurrentWebview } from '@tauri-apps/api/webview'
import { probeFile, fileNameOf, type Source } from '../engine/source'
import { convertToWav, inspectMedia, MEDIA_EXTENSIONS, NATIVE_EXTENSIONS, type Conversion } from '../engine/converter'
import { removeScratchFile } from '../engine/engine'

interface UploadStepProps {
  onSourceReady: (source: Source) => void
}

type Phase =
  | { kind: 'idle' }
  | { kind: 'reading' }
  | { kind: 'inspecting'; name: string }
  | { kind: 'converting'; name: string; fraction: number; fromVideo: boolean }

const extensionOf = (path: string) => (path.toLowerCase().match(/\.([a-z0-9]+)$/)?.[1] ?? '')

export default function UploadStep({ onSourceReady }: UploadStepProps) {
  const [isDragging, setIsDragging] = useState(false)
  const [phase, setPhase] = useState<Phase>({ kind: 'idle' })
  const [error, setError] = useState<string | null>(null)
  const conversionRef = useRef<Conversion | null>(null)
  const cancelledRef = useRef(false)
  const busy = phase.kind !== 'idle'

  const handlePath = async (path: string | undefined) => {
    if (!path || busy) return
    const name = fileNameOf(path)
    setError(null)
    cancelledRef.current = false

    // WAV and MP3 are read directly. If that fails (an unusual WAV, say), or for any other kind of
    // file (M4A, FLAC, video ...), convert to a WAV first.
    if (NATIVE_EXTENSIONS.includes(extensionOf(path))) {
      setPhase({ kind: 'reading' })
      try {
        onSourceReady(await probeFile(path))
        return
      } catch {
        /* fall through to conversion */
      }
    }

    let outPath: string | null = null
    try {
      setPhase({ kind: 'inspecting', name })
      const info = await inspectMedia(path)
      if (cancelledRef.current) throw new Error('Cancelled')
      if (!info.hasAudio) throw new Error('There doesn’t seem to be any audio in this file.')
      setPhase({ kind: 'converting', name, fraction: 0, fromVideo: info.hasVideo })
      const conversion = await convertToWav(path, info, (fraction) =>
        setPhase((p) => (p.kind === 'converting' ? { ...p, fraction } : p)),
      )
      conversionRef.current = conversion
      outPath = conversion.outPath
      await conversion.done
      if (cancelledRef.current) throw new Error('Cancelled')
      setPhase({ kind: 'reading' })
      const source = await probeFile(conversion.outPath, name)
      onSourceReady({ ...source, scratchFile: conversion.outPath })
    } catch (err) {
      if (outPath) void removeScratchFile(outPath)
      if (!(err instanceof Error && err.message === 'Cancelled')) {
        setError(err instanceof Error ? err.message : 'Could not read this file.')
      }
      setPhase({ kind: 'idle' })
    } finally {
      conversionRef.current = null
    }
  }

  const cancel = () => {
    cancelledRef.current = true
    conversionRef.current?.job.cancel()
  }

  const browse = async () => {
    const chosen = await open({
      multiple: false,
      filters: [
        { name: 'Audio and video', extensions: MEDIA_EXTENSIONS },
        { name: 'All files', extensions: ['*'] },
      ],
    })
    if (typeof chosen === 'string') await handlePath(chosen)
  }

  // Files dropped on the window arrive as real paths (the helpers read them directly). If
  // drag-and-drop isn't available for any reason, clicking to browse still works.
  useEffect(() => {
    let unlisten: (() => void) | undefined
    let cancelled = false
    try {
      void getCurrentWebview()
        .onDragDropEvent((event) => {
          const p = event.payload
          if (p.type === 'over' || p.type === 'enter') setIsDragging(true)
          else if (p.type === 'leave') setIsDragging(false)
          else if (p.type === 'drop') {
            setIsDragging(false)
            void handlePath(p.paths[0])
          }
        })
        .then((fn) => {
          if (cancelled) fn()
          else unlisten = fn
        })
        .catch(() => {})
    } catch {
      /* drag-and-drop unavailable */
    }
    return () => {
      cancelled = true
      unlisten?.()
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [busy])

  // Development only: ?mock=converting shows the conversion screen against the fake helpers.
  useEffect(() => {
    if (import.meta.env.DEV && new URLSearchParams(window.location.search).get('mock') === 'converting') {
      void handlePath('/Users/demo/Sermons/2026.10.25 Sunday service.mp4')
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  return (
    <div className="upload-step">
      <div
        className={`upload-drop${isDragging ? ' dragging' : ''}`}
        onClick={() => !busy && void browse()}
        onKeyDown={(e) => {
          if (!busy && (e.key === 'Enter' || e.key === ' ')) {
            e.preventDefault()
            void browse()
          }
        }}
        role="button"
        tabIndex={0}
      >
        {phase.kind === 'idle' && (
          <>
            <p className="upload-drop-title">Drop a sermon recording here</p>
            <p className="upload-drop-subtitle">or click to browse — audio or video, almost any format (MP3, WAV, M4A, MP4, MOV …)</p>
          </>
        )}
        {phase.kind === 'reading' && (
          <>
            <p className="upload-drop-title">Reading your recording…</p>
            <p className="upload-drop-subtitle">Long recordings take a few seconds.</p>
          </>
        )}
        {phase.kind === 'inspecting' && <p className="upload-drop-title">Looking at {phase.name}…</p>}
        {phase.kind === 'converting' && (
          <>
            <p className="upload-drop-title">
              {phase.fromVideo ? 'Taking the audio out of this video…' : 'Converting to a format the cleaner can read…'} {Math.round(phase.fraction * 100)}%
            </p>
            <div className="progress-bar" role="progressbar" aria-valuemin={0} aria-valuemax={100} aria-valuenow={Math.round(phase.fraction * 100)}>
              <div className="progress-fill" style={{ width: `${Math.max(2, phase.fraction * 100)}%` }} />
            </div>
            <p className="upload-drop-subtitle">{phase.name}</p>
          </>
        )}
        {error && <p className="upload-error">{error}</p>}
      </div>
      {phase.kind === 'converting' && (
        <button type="button" onClick={cancel}>
          Cancel
        </button>
      )}

      <ul className="upload-disclaimers">
        <li>Your recording never leaves this computer — everything happens right here.</li>
        <li>How long cleaning takes depends on your computer and the length of the recording. A 45-minute sermon usually takes a few minutes.</li>
      </ul>
    </div>
  )
}
