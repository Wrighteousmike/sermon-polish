import { useEffect, useState } from 'react'
import type { Step } from './types'
import UploadStep from './steps/UploadStep'
import Studio from './steps/Studio'
import ExportStep, { type ExportRequest } from './steps/ExportStep'
import OneflockWordmark from './components/OneflockWordmark'
import Rail from './components/Rail'
import { clearScratch } from './engine/engine'
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
