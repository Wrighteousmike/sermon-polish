import { useEffect, useState } from 'react'
import type { Step } from './types'
import UploadStep from './steps/UploadStep'
import Studio from './steps/Studio'
import ExportStep, { type ExportRequest } from './steps/ExportStep'
import OneflockWordmark from './components/OneflockWordmark'
import Rail from './components/Rail'
import { clearScratch, logLine, readDiagnostics, openReleasePage } from './engine/engine'
import { getVersion } from '@tauri-apps/api/app'
import { checkForUpdate, type UpdateInfo } from './lib/update'
import { listen } from '@tauri-apps/api/event'
import type { Source } from './engine/source'
import { STANDARD } from './engine/settings'
import { mockSource } from './mockData'
import './App.css'

// Development only: ?mock=<screen> starts on that screen with fake data.
const mockScreen = import.meta.env.DEV ? new URLSearchParams(window.location.search).get('mock') : null

function App() {
  const [step, setStep] = useState<Step>(
    mockScreen === 'download' ? 'download' : mockScreen === 'trim' ? 'trim' : mockScreen === 'cleanup' || mockScreen === 'processing' ? 'plugins' : 'upload',
  )
  const [source, setSource] = useState<Source | null>(mockScreen && mockScreen !== 'upload' ? mockSource : null)
  // What the studio hands to the export screen (null until Export is pressed).
  const [exportRequest, setExportRequest] = useState<ExportRequest | null>(
    mockScreen === 'download' ? { settings: STANDARD, trim: { start: 12, end: 2662 }, render: () => ({ done: Promise.resolve({ seconds: 2650 }), cancel: () => {} }) } : null,
  )

  // The "Cleaning up" dialog: shown while the temporary files are deleted, on Start again and when quitting.
  const [cleaning, setCleaning] = useState<'again' | 'quit' | null>(null)
  useEffect(() => {
    let off: (() => void) | undefined
    let gone = false
    listen('quitting', () => setCleaning('quit'))
      .then((un) => (gone ? un() : (off = un)))
      .catch(() => {})
    return () => {
      gone = true
      off?.()
    }
  }, [])

  // Diagnostics: notes if the window stalls or errors, and "Copy diagnostics" collects the log for a bug report.
  useEffect(() => {
    let last = performance.now()
    const id = window.setInterval(() => {
      const now = performance.now()
      const lag = now - last - 500
      if (lag > 1500) logLine(`ui: the window stalled for ${Math.round(lag)} ms (it was busy)`)
      last = now
    }, 500)
    // A diagnostic build also notes every 2 s that the window is alive; if these stop, the window is frozen.
    const beat = import.meta.env.VITE_DIAG === '1' ? window.setInterval(() => logLine('ui: heartbeat'), 2000) : undefined
    const onError = (e: ErrorEvent) => logLine(`ui error: ${e.message}`)
    const onReject = (e: PromiseRejectionEvent) => logLine(`ui error (promise): ${String(e.reason).slice(0, 300)}`)
    window.addEventListener('error', onError)
    window.addEventListener('unhandledrejection', onReject)
    return () => {
      window.clearInterval(id)
      if (beat) window.clearInterval(beat)
      window.removeEventListener('error', onError)
      window.removeEventListener('unhandledrejection', onReject)
    }
  }, [])
  const [diag, setDiag] = useState<{ text: string; copied: boolean } | null>(null)
  const copyDiagnostics = async () => {
    const text = await readDiagnostics().catch((e) => `Could not read the log: ${e}`)
    let copied = false
    try {
      await navigator.clipboard.writeText(text)
      copied = true
    } catch {
      copied = false
    }
    setDiag({ text, copied })
  }

  // Update check: once a day at start-up (quietly), and on request from the sidebar.
  const [update, setUpdate] = useState<UpdateInfo | null>(null)
  const [updateNote, setUpdateNote] = useState<string | null>(null) // result of a manual check, shown in a dialog
  const [checking, setChecking] = useState(false)
  const runUpdateCheck = async (manual: boolean) => {
    if (manual) setChecking(true)
    try {
      const current = await getVersion().catch(() => '0.0.0')
      const found = await checkForUpdate(current)
      logLine(`update check: ${found ? `version ${found.version} is available` : 'up to date'} (running ${current})`)
      setUpdate(found)
      if (manual) setUpdateNote(found ? '' : `You have the latest version (${current}).`)
      try { localStorage.setItem('update-checked', String(Date.now())) } catch { /* fine without it */ }
    } catch (err) {
      logLine(`update check failed: ${err instanceof Error ? err.message : String(err)}`)
      if (manual) setUpdateNote('Couldn’t check for updates. Check your internet connection and try again.')
    } finally {
      if (manual) setChecking(false)
    }
  }
  useEffect(() => {
    let last = 0
    try { last = Number(localStorage.getItem('update-checked') ?? 0) } catch { last = 0 }
    if (Date.now() - last < 24 * 3600 * 1000 && !(import.meta.env.DEV && new URLSearchParams(window.location.search).get('fakeupdate'))) return
    const t = setTimeout(() => void runUpdateCheck(false), 4000)
    return () => clearTimeout(t)
  }, []) // eslint-disable-line react-hooks/exhaustive-deps
  const openUpdate = (u: UpdateInfo) => void openReleasePage(u.url).catch(() => {})

  const startAgain = async () => {
    setCleaning('again')
    // Stops the engine and deletes every intermediate file; the dialog stays up at least a moment so it can be read.
    await Promise.all([clearScratch().catch(() => {}), new Promise((r) => setTimeout(r, 900))])
    setExportRequest(null)
    setSource(null)
    setStep('upload')
    setCleaning(null)
  }

  return (
    <div className="app">
      <aside className="sidebar">
        <h1 className="app-title">
          <OneflockWordmark />
          <span>Sermon Polish</span>
        </h1>
        <div id="sidebar-steps">
          {!source && (
            <Rail
              items={[
                { id: 'open', n: 1, title: 'Open recording', sub: 'Choose a file', state: 'on', current: true },
                { id: 'trim', n: 2, title: 'Top and tail', sub: 'Set the start and end', state: 'todo' },
                { id: 'clean', n: 3, title: 'Clean up', sub: 'Auto, then your changes', state: 'todo' },
                { id: 'export', n: 4, title: 'Export…', sub: 'MP3 or WAV', state: 'todo' },
              ]}
            />
          )}
        </div>
        {update && (
          <div className="update-card" role="status">
            <strong>Version {update.version} is available</strong>
            <span>A newer Sermon Polish is on the download page.</span>
            <button type="button" className="primary" onClick={() => openUpdate(update)}>Get the update</button>
          </div>
        )}
        <button type="button" className="link-button sidebar-diag" disabled={checking} onClick={() => void runUpdateCheck(true)}>{checking ? 'Checking…' : 'Check for updates'}</button>
        <button type="button" className="link-button sidebar-diag" onClick={() => void copyDiagnostics()} title="Copies a short technical log to paste into a bug report">Copy diagnostics</button>
      </aside>

      <main className="app-main">
        {step === 'upload' && (
          <UploadStep
            onSourceReady={(s) => {
              setSource(s)
              setStep('trim')
            }}
          />
        )}
        {source && step !== 'upload' && (
          // The studio stays mounted (just hidden) while exporting, so Back returns to the same settings and trim.
          <div hidden={step === 'download'}>
            <Studio
              source={source}
              paused={step === 'download'}
              onStartAgain={startAgain}
              onReturn={() => setStep('plugins')}
              onExport={(request) => {
                setExportRequest(request)
                setStep('download')
              }}
            />
          </div>
        )}
        {step === 'download' && exportRequest && source && (
          <ExportStep source={source} request={exportRequest} onBack={() => setStep('plugins')} />
        )}
      </main>
      {updateNote !== null && (
        <div className="modal-backdrop" role="presentation">
          <div className="modal-card" role="dialog" aria-modal="true" aria-labelledby="update-title">
            <h2 id="update-title">{update ? `Version ${update.version} is available` : 'Updates'}</h2>
            <p>{update ? 'A newer Sermon Polish is on the download page. Download it and install it over this one; your settings are kept.' : updateNote}</p>
            <div className="modal-actions">
              <button type="button" onClick={() => setUpdateNote(null)}>{update ? 'Later' : 'Close'}</button>
              {update && <button type="button" className="primary" autoFocus onClick={() => { openUpdate(update); setUpdateNote(null) }}>Get the update</button>}
            </div>
          </div>
        </div>
      )}
      {diag && (
        <div className="modal-backdrop" role="presentation">
          <div className="modal-card" role="dialog" aria-modal="true" aria-labelledby="diag-title" style={{ width: 'min(640px, 100%)' }}>
            <h2 id="diag-title">{diag.copied ? 'Diagnostics copied' : 'Diagnostics'}</h2>
            <p>{diag.copied ? 'Paste it into your message. It contains timings and messages from the app, not your audio.' : 'Select all of this and copy it into your message. It contains timings and messages from the app, not your audio.'}</p>
            <textarea readOnly value={diag.text} rows={10} style={{ width: '100%', boxSizing: 'border-box', fontFamily: 'ui-monospace, Consolas, monospace', fontSize: '0.75rem' }} onFocus={(e) => e.currentTarget.select()} />
            <div className="modal-actions" style={{ marginTop: 12 }}>
              <button type="button" onClick={() => void navigator.clipboard.writeText(diag.text).then(() => setDiag({ ...diag, copied: true })).catch(() => {})}>Copy again</button>
              <button type="button" className="primary" autoFocus onClick={() => setDiag(null)}>Close</button>
            </div>
          </div>
        </div>
      )}
      {cleaning && (
        <div className="modal-backdrop" role="presentation">
          <div className="modal-card" role="alertdialog" aria-modal="true" aria-labelledby="cleaning-title" aria-describedby="cleaning-body">
            <h2 id="cleaning-title">
              <span className="applying measuring"><i className="spinner" aria-hidden /> Cleaning up…</span>
            </h2>
            <p id="cleaning-body">{cleaning === 'quit' ? 'Removing the temporary files before Sermon Polish closes. This only takes a moment.' : 'Removing the temporary files from this recording. This only takes a moment.'}</p>
          </div>
        </div>
      )}
    </div>
  )
}

export default App
