// The single-screen studio: play the recording through the live chain, change any setting while it
// plays, trim on the waveform, and export. Audio is played by the native engine (`sermon_host`);
// this component only sends it commands and draws what it reports.
import { useCallback, useEffect, useRef, useState } from 'react'
import { createPortal } from 'react-dom'
import WaveSurfer from 'wavesurfer.js'
import RegionsPlugin, { type Region } from 'wavesurfer.js/plugins/regions'
import { convertFileSrc } from '@tauri-apps/api/core'
import { runEngine, scratchPath, removeScratchFile, type EngineJob } from '../engine/engine'
import { liveParams, startHost, type HostConnection, type HostState } from '../engine/host'
import {
  amountLabel,
  applySimple,
  MARKS,
  sameSettings,
  sameValue,
  SIMPLE_SLIDERS,
  simplePosition,
  type SimpleSlider,
  settingsFromSuggestions,
  STAGES,
  STANDARD,
  suggestedValue,
  type Analysis,
  type Settings,
  type StageDef,
  type Suggestion,
} from '../engine/settings'
import type { Source } from '../engine/source'
import { formatTime } from '../lib/time'
import AmountSlider from '../components/AmountSlider'
import TrimDetail from '../components/TrimDetail'
import Tour, { type TourStep } from '../components/Tour'
import Rail, { type RailItem } from '../components/Rail'
import type { ExportRequest, Rendering } from './ExportStep'

const CONFIDENCE_LABEL: Record<Suggestion['confidence'], string> = { high: 'confident', medium: 'fairly sure', low: 'rough guess' }
// Stages the live chain can't run block by block: the engine bakes them in the background first.
const BAKED: (keyof Settings)[] = ['declip', 'dereverb']

// Whether the tour has already been offered in this session (in case browser storage isn't available).
let tourShownThisSession = false

type AnalysisState = 'idle' | 'running' | 'done' | 'failed'

const meterPct = (db: number, floor = -60) => `${Math.max(0, Math.min(100, ((db - floor) / -floor) * 100))}%`

export default function Studio({ source, paused, onExport, onStartAgain, onReturn }: { source: Source; paused: boolean; onExport: (r: ExportRequest) => void; onStartAgain: () => void; onReturn: () => void }) {
  const waveRef = useRef<HTMLDivElement>(null)
  const wsRef = useRef<WaveSurfer | null>(null)
  const regionRef = useRef<Region | null>(null)
  const hostRef = useRef<HostConnection | null>(null)

  const [loading, setLoading] = useState<{ label: string; fraction: number } | null>({ label: 'Opening…', fraction: 0 })
  const [error, setError] = useState<string | null>(null)
  const [host, setHost] = useState<HostState | null>(null)
  const [ready, setReady] = useState(false)
  const [original, setOriginal] = useState(false)
  const [loop, setLoop] = useState(false)
  const [region, setRegion] = useState({ start: 0, end: source.duration })
  const [showFine, setShowFine] = useState(true)
  // Which step of the left rail is showing: the start/end controls, or the cleaning sliders.
  const [tab, setTab] = useState<'trim' | 'clean'>(() => (import.meta.env.DEV && ['cleanup', 'processing', 'download'].includes(new URLSearchParams(window.location.search).get('mock') ?? '') ? 'clean' : 'trim'))
  const tabBeforeTour = useRef<'trim' | 'clean'>('trim')
  const [lastChanged, setLastChanged] = useState<keyof Settings | null>(null)
  const [view, setView] = useState<'simple' | 'advanced'>(() => {
    try {
      return localStorage.getItem('studio-view') === 'advanced' ? 'advanced' : 'simple'
    } catch {
      return 'simple'
    }
  })
  const chooseView = (v: 'simple' | 'advanced') => {
    setView(v)
    try {
      localStorage.setItem('studio-view', v)
    } catch {
      /* not remembered */
    }
  }
  const renderRef = useRef<{ onProgress: (l: string, f: number) => void; resolve: (r: { seconds: number }) => void; reject: (e: Error) => void } | null>(null)
  const [baking, setBaking] = useState(false)
  const [confirmRestart, setConfirmRestart] = useState(false)
  // Export always waits for Auto to finish listening, so the file is made with the settings it chose.
  const [waitingForAuto, setWaitingForAuto] = useState(false)
  // First-time tour: shown the first time a recording is opened, and again whenever asked for.
  const [tourOpen, setTourOpen] = useState(false)
  const fineBeforeTour = useRef(false)
  const [solvedGain, setSolvedGain] = useState<number | null>(null)
  // The most the limiter has reduced the peaks since the settings last changed (what the pre-gain is tuned against).
  const [limiterPeak, setLimiterPeak] = useState(0)

  const [analysisState, setAnalysisState] = useState<AnalysisState>('idle')
  const [analysis, setAnalysis] = useState<Analysis | null>(null)
  const [settings, setSettings] = useState<Settings>(STANDARD)
  const [touched, setTouched] = useState(false)
  // Nothing heavy runs (Auto, echo reduction, clip repair) until the start and end are saved, so the work is
  // done once, on the part that is kept, rather than again for every nudge of a trim handle.
  const [trimSaved, setTrimSaved] = useState(() => import.meta.env.DEV && ['cleanup', 'processing', 'download'].includes(new URLSearchParams(window.location.search).get('mock') ?? ''))
  const touchedRef = useRef(touched)
  touchedRef.current = touched


  // ---- engine connection ----
  useEffect(() => {
    let alive = true
    if (import.meta.env.DEV && new URLSearchParams(window.location.search).get('mock')) {
      // Development preview without the native engine: pretend it is playing.
      setLoading(null)
      setReady(true)
      setHost({ position: source.duration * 0.3, playing: true, atEnd: false, applying: false, peakDb: -9, momentary: -14, shortTerm: -14.2, leveler: 2.4, comp: 3.1, deess: 1.2, limiter: 0.6, underruns: 0, load: 0.03 })
      return
    }
    void (async () => {
      const stamp = Date.now()
      const work = await scratchPath(`work-${stamp}.wav`)
      const peaks = await scratchPath(`work-${stamp}.spk`)
      if (!alive) return
      const conn = startHost(
        {
          onState: (s) => alive && setHost(s),
          onProgress: (label, fraction) => alive && setLoading({ label, fraction }),
          onLoudness: (gain) => alive && setSolvedGain(gain),
          onBake: (state) => alive && setBaking(state === 'working'),
          onRender: (ev) => {
            const r = renderRef.current
            if (!r) return
            if (ev.kind === 'progress') r.onProgress(ev.label ?? '', ev.fraction ?? 0)
            else {
              renderRef.current = null
              if (ev.kind === 'done') r.resolve({ seconds: ev.seconds ?? 0 })
              else r.reject(new Error(ev.message === 'Cancelled' ? 'Cancelled' : ev.message ?? 'The export failed.'))
            }
          },
          onError: (m) => alive && setError(m),
          onExit: () => alive && setError('The audio engine stopped unexpectedly.'),
        },
        { path: source.path, work, peaks },
      )
      hostRef.current = conn
      conn.opened
        .then(() => {
          if (!alive) return
          setLoading(null)
          setReady(true)
        })
        .catch((e: Error) => alive && setError(e.message))
    })()
    return () => {
      alive = false
      hostRef.current?.close()
      hostRef.current = null
    }
  }, [source])

  const send = useCallback((cmd: Record<string, unknown>) => hostRef.current?.send(cmd), [])

  // ---- waveform + trim region (display only; the engine plays) ----
  useEffect(() => {
    if (!waveRef.current) return
    const regions = RegionsPlugin.create()
    const ws = WaveSurfer.create({
      container: waveRef.current,
      url: convertFileSrc(source.path),
      peaks: [source.peaks],
      duration: source.duration,
      waveColor: '#a9bccb',
      progressColor: '#3b4756',
      cursorColor: '#d9480f',
      height: 120,
      normalize: true,
      interact: false,
      plugins: [regions],
    })
    ws.setMuted(true)
    wsRef.current = ws
    ws.on('ready', () => {
      regionRef.current = regions.addRegion({ start: 0, end: source.duration, drag: false, resize: true, color: 'rgba(61, 90, 115, 0.16)' })
    })
    regions.on('region-updated', (r) => setRegion({ start: r.start, end: r.end }))
    // Clicking the waveform (outside the handles) moves the engine's playhead.
    const el = waveRef.current
    const onClick = (e: MouseEvent) => {
      if ((e.target as HTMLElement).closest('[part~="region-handle"], .wavesurfer-handle')) return
      const rect = el.getBoundingClientRect()
      const t = Math.max(0, Math.min(1, (e.clientX - rect.left) / rect.width)) * source.duration
      hostRef.current?.send({ cmd: 'seek', t })
    }
    el.addEventListener('click', onClick)
    return () => {
      el.removeEventListener('click', onClick)
      ws.destroy()
      wsRef.current = null
      regionRef.current = null
    }
  }, [source])

  // The start and end handles only move while the Top and tail step is showing.
  useEffect(() => {
    regionRef.current?.setOptions({ resize: tab === 'trim' })
  }, [tab, ready])

  // The engine's playhead drives the waveform cursor.
  useEffect(() => {
    if (host && wsRef.current && source.duration > 0) wsRef.current.setTime(host.position)
  }, [host?.position, source.duration]) // eslint-disable-line react-hooks/exhaustive-deps

  // Trim changes keep playback inside the kept range.
  useEffect(() => {
    if (ready) send({ cmd: 'trim', start: region.start, end: region.end })
  }, [ready, region.start, region.end, send])
  useEffect(() => {
    if (ready) send({ cmd: 'loop', on: loop })
  }, [ready, loop, send])
  useEffect(() => {
    if (ready) send({ cmd: 'ab', original })
  }, [ready, original, send])

  // ---- Auto analysis of the kept region ----
  useEffect(() => {
    if (!trimSaved) {
      setAnalysisState('idle')
      return
    }
    let cancelled = false
    setAnalysisState('running')
    let job: EngineJob | null = null
    const timer = setTimeout(() => {
      job = runEngine([source.path, '--analyze', '--trim', String(region.start), String(region.end), '--json'])
      job.done
        .then((ev) => {
          if (cancelled) return
          const a = ev as unknown as Analysis
          setAnalysis(a)
          setAnalysisState('done')
          if (!touchedRef.current) setSettings((prev) => ({ ...settingsFromSuggestions(a.suggestions), pregain: prev.pregain }))
        })
        .catch((err: Error) => {
          if (cancelled || err.message === 'Cancelled') return
          setAnalysisState('failed')
          if (!touchedRef.current) setSettings((prev) => ({ ...STANDARD, pregain: prev.pregain }))
        })
    }, 700) // wait for trim drags to settle
    return () => {
      cancelled = true
      clearTimeout(timer)
      job?.cancel()
    }
  }, [source.path, region.start, region.end, trimSaved])

  useEffect(() => {
    const r = host?.limiter ?? 0
    if (r > limiterPeak) setLimiterPeak(r)
  }, [host?.limiter]) // eslint-disable-line react-hooks/exhaustive-deps
  useEffect(() => setLimiterPeak(0), [settings])

  // ---- live settings ----
  // Dragging a slider changes settings many times a second; send at most ~15 updates a second (the first
  // at once, the last always), which is plenty for the engine to follow.
  const lastSent = useRef(0)
  useEffect(() => {
    if (!ready) return
    const wait = Math.max(0, lastSent.current + 65 - Date.now())
    const t = setTimeout(() => {
      lastSent.current = Date.now()
      // Until the trim is saved, play with the cheap parts of the chain only (no echo reduction or clip repair to prepare).
      send(liveParams(trimSaved ? settings : { ...settings, dereverb: 0, declip: false }))
    }, wait)
    return () => clearTimeout(t)
  }, [ready, settings, send, trimSaved])

  const suggestionFor = (def: StageDef): Suggestion | undefined => analysis?.suggestions.find((s) => s.stage === def.stage && suggestedValue(def, s) !== undefined)
  const suggested = analysis ? settingsFromSuggestions(analysis.suggestions) : null
  const custom = touched && (suggested ? !sameSettings(settings, suggested) : true)
  const change = <K extends keyof Settings>(key: K, value: Settings[K]) => {
    setSettings((prev) => ({ ...prev, [key]: value }))
    setTouched(true)
    setLastChanged(key)
  }

  // ---- transport ----
  const playing = host?.playing ?? false
  // Nothing plays until the engine has measured the loudness: before that the output level is still being
  // worked out and would jump (by tens of dB on a quiet recording) in the middle of listening.
  const canPlay = ready && solvedGain !== null
  // Pressing a play button before the loudness is measured says why nothing happened, rather than doing nothing.
  const [blockedNote, setBlockedNote] = useState(false)
  const blockedTimer = useRef<number | undefined>(undefined)
  const notifyBlocked = () => {
    setBlockedNote(true)
    window.clearTimeout(blockedTimer.current)
    blockedTimer.current = window.setTimeout(() => setBlockedNote(false), 2800)
  }
  // The play buttons visibly come alive (one soft pulse) the moment playback unlocks.
  const [justReady, setJustReady] = useState(false)
  useEffect(() => {
    if (!canPlay) return
    setBlockedNote(false)
    setJustReady(true)
    const t = window.setTimeout(() => setJustReady(false), 1400)
    return () => window.clearTimeout(t)
  }, [canPlay])
  const togglePlay = useCallback(() => {
    if (playing) send({ cmd: 'pause' })
    else if (canPlay) send({ cmd: 'play' })
    else notifyBlocked()
  }, [playing, canPlay, send])
  const hearFrom = (t: number) => {
    if (!canPlay) {
      notifyBlocked()
      return
    }
    send({ cmd: 'seek', t })
    send({ cmd: 'play' })
  }
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (paused || tourOpen) return
      const tag = (e.target as HTMLElement).tagName
      if (tag === 'INPUT' || tag === 'TEXTAREA') return
      if (e.code === 'Space') {
        e.preventDefault()
        togglePlay()
      } else if (e.key === 'i' || e.key === 'I') setMarker('start', host?.position ?? 0)
      else if (e.key === 'o' || e.key === 'O') setMarker('end', host?.position ?? source.duration)
      else if (e.key === 'a' || e.key === 'A') setOriginal((v) => !v)
      else if (e.key === 'l' || e.key === 'L') setLoop((v) => !v)
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }) // eslint-disable-line react-hooks/exhaustive-deps

  const setMarker = (which: 'start' | 'end', t: number) => {
    const next = which === 'start' ? { start: Math.max(0, Math.min(t, region.end - 0.5)), end: region.end } : { start: region.start, end: Math.min(source.duration, Math.max(t, region.start + 0.5)) }
    regionRef.current?.setOptions(next)
    setRegion(next)
  }

  const startTour = useCallback(() => {
    fineBeforeTour.current = showFine
    tabBeforeTour.current = tab
    send({ cmd: 'pause' })
    setTourOpen(true)
  }, [send, showFine, tab])
  const endTour = () => {
    setTourOpen(false)
    setShowFine(fineBeforeTour.current)
    setTab(tabBeforeTour.current)
    try {
      localStorage.setItem('tour-seen', '1')
    } catch {
      /* the tour may show again next time */
    }
  }
  useEffect(() => {
    if (!ready || paused) return
    let seen = false
    try {
      seen = localStorage.getItem('tour-seen') === '1'
    } catch {
      seen = false
    }
    if (!seen && !tourShownThisSession) {
      tourShownThisSession = true
      const t = setTimeout(startTour, 900) // let the waveform draw first
      return () => clearTimeout(t)
    }
  }, [ready]) // eslint-disable-line react-hooks/exhaustive-deps

  const tourSteps: TourStep[] = [
    {
      target: 'waveform',
      before: () => setTab('trim'),
      title: 'Your recording',
      body: (
        <>
          <p>This is the whole recording. The lighter band between the two handles is the part that will be kept.</p>
          <p>Drag a handle to roughly set where the sermon starts and ends. Click anywhere on the waveform to move the playhead there, and press <kbd>Space</kbd> to play.</p>
        </>
      ),
    },
    {
      target: 'fine-start',
      before: () => setTab('trim'),
      title: 'Top and tail: use the zoomed views',
      body: (
        <>
          <p>Dragging the handles on the whole recording is only approximate; at this size one pixel can be several seconds.</p>
          <p>The two <strong>zoomed views</strong> below the waveform let you cut exactly where the sermon begins and finishes. Start with the beginning.</p>
        </>
      ),
    },
    {
      target: 'fine-start',
      before: () => setShowFine(true),
      title: 'Setting the start',
      body: (
        <>
          <p>The view is centred on the start marker (the blue line). Everything greyed to its left will be cut.</p>
          <ul>
            <li><strong>− / +</strong> zoom in and out. Start at about 15 s, then zoom to 1–2 s to find the first word exactly.</li>
            <li><strong>Click or drag</strong> in the waveform to place the marker, or nudge it with the <strong>±1 s</strong> and <strong>±0.1 s</strong> buttons. With the waveform selected, the arrow keys move it 0.1 s (<kbd>Shift</kbd> 1 s, <kbd>Alt</kbd> 0.01 s).</li>
            <li><strong>Normalise</strong> scales the picture so a quiet start or end is easy to see. It doesn’t change the sound.</li>
          </ul>
          <p>Put the marker just <em>before</em> the first word, leaving a breath or a moment of room tone, not on the first syllable. A short fade is added for you so there’s no click.</p>
        </>
      ),
    },
    {
      target: 'fine-start',
      title: 'Check it by listening',
      body: (
        <>
          <p><strong>Hear from here</strong> plays from the marker, so you can tell straight away if the first word is clipped or there’s too much dead air.</p>
          <p><strong>Set to playhead</strong> is the quickest way: play the recording, and when the sermon starts press <kbd>I</kbd> (or click the button) to drop the start marker right there, then fine-tune it.</p>
        </>
      ),
    },
    {
      target: 'fine-end',
      before: () => setShowFine(true),
      title: 'Setting the end',
      body: (
        <>
          <p>The end works the same way, but this time everything to the <em>right</em> of the marker is cut.</p>
          <ul>
            <li>Place it after the last word has finished, including the “Amen” and any natural decay of the room, but before any chatter, music or clapping you don’t want.</li>
            <li><strong>Hear up to here</strong> plays the last few seconds into the marker so you can check the ending sounds finished, not chopped off.</li>
            <li>Press <kbd>O</kbd> while playing to set the end at the playhead.</li>
          </ul>
          <p>The cleaning and loudness are worked out for the kept part only, so top and tail first, then press <strong>Save trim</strong> and Auto starts.</p>
        </>
      ),
    },
    {
      target: 'save-trim',
      before: () => setTab('trim'),
      title: 'Save the trim to begin',
      body: (
        <>
          <p>When the start and end are right, press <strong>Save trim and continue</strong>. Auto listens to the kept part and the Clean up step opens with its sliders. Nothing heavy runs before this, so your computer isn’t busy with audio you’re about to cut.</p>
          <p>Need to adjust it later? Click <strong>Top and tail</strong> in the list on the left, change it, and save again.</p>
        </>
      ),
    },
    {
      target: 'transport',
      title: 'Listen while you work',
      body: (
        <>
          <p><strong>Play</strong> and change any setting while it plays; you’ll hear the change within a moment. <strong>Cleaned / Original</strong> flips between the processed and untouched audio (press <kbd>A</kbd>), and <strong>Loop</strong> (<kbd>L</kbd>) repeats the kept part.</p>
          <p>When this spinner shows <strong>Processing…</strong>, something is still being prepared in the background; it lists what, and you’ll hear each part shortly.</p>
        </>
      ),
    },
    {
      target: 'auto',
      before: () => setTab('clean'),
      title: 'Auto picks a starting point',
      body: (
        <>
          <p>The app listens to the part you’ve kept and suggests a setting for each step, with the reason. It re-checks whenever you change the start or end.</p>
          <p>Change anything you like. <strong>Reset to Auto</strong> brings the suggestions back.</p>
        </>
      ),
    },
    {
      target: 'sliders',
      title: 'Sliders, not presets',
      body: (
        <>
          <p>Every step is a slider from <strong>Off</strong> to <strong>Max</strong>. The small orange tick shows where Auto put it. <strong>Simple</strong> gives you three sliders; <strong>Advanced</strong> has one for every step.</p>
          <p>Room echo takes a minute or two to prepare the first time you raise it. After that, the slider is instant.</p>
        </>
      ),
    },
    {
      target: 'export',
      title: 'When you’re happy',
      body: (
        <>
          <p><strong>Export…</strong> saves only the part you kept, at streaming loudness with the peaks held at −1 dB, as a WAV or MP3.</p>
          <p>You can replay this tour any time from <strong>Take the tour</strong> at the bottom of the list on the left.</p>
        </>
      ),
    },
  ]

  // Pausing while the export screen is open; Export itself happens there.
  useEffect(() => {
    if (paused) send({ cmd: 'pause' })
  }, [paused, send])
  const renderCache = useRef<{ key: string; path: string } | null>(null)
  const handleExport = () => {
    send({ cmd: 'pause' })
    send(liveParams(settings)) // make sure the engine has exactly the settings shown
    const trim = { start: region.start, end: region.end }
    const render = (out: string, onProgress: (label: string, fraction: number) => void): Rendering => ({
      done: new Promise((resolve, reject) => {
        renderRef.current = { onProgress, resolve, reject }
        send({ cmd: 'render', out, start: trim.start, end: trim.end })
      }),
      cancel: () => send({ cmd: 'cancelRender' }),
    })
    // The cleaned file from the last export is kept: opening Export again with nothing changed reuses it
    // instead of cleaning the whole recording a second time.
    const key = JSON.stringify({ settings, trim })
    const hit = renderCache.current?.key === key ? renderCache.current.path : null
    if (!hit && renderCache.current) void removeScratchFile(renderCache.current.path)
    if (!hit) renderCache.current = null
    onExport({ settings, trim, render, cached: hit, onRendered: (path) => { renderCache.current = { key, path } } })
  }

  const requestExport = () => {
    send({ cmd: 'pause' })
    if (analysisState === 'running' || analysisState === 'idle') setWaitingForAuto(true)
    else handleExport()
  }
  // When Auto finishes (or fails, which falls back to the standard settings), carry on to the export.
  useEffect(() => {
    if (waitingForAuto && analysisState !== 'running' && analysisState !== 'idle') {
      setWaitingForAuto(false)
      handleExport()
    }
  }, [waitingForAuto, analysisState]) // eslint-disable-line react-hooks/exhaustive-deps

  const changeAll = (next: Settings) => {
    setSettings(next)
    setTouched(true)
  }

  const renderSimple = (sl: SimpleSlider) => {
    const pos = simplePosition(sl.id, settings)
    const stageDef = STAGES.find((d) => d.stage === sl.stage)!
    const g = suggestionFor(stageDef)
    const autoAt = g !== undefined ? suggestedValue(stageDef, g) : undefined
    const matches = g !== undefined && sameValue(settings[stageDef.key], autoAt)
    const applying = host?.applying && lastChanged === stageDef.key
    return (
      <li key={sl.id} className="plugin-row simple-row">
        <div className="plugin-header">
          <label className="plugin-label" htmlFor={`simple-${sl.id}`}>{sl.label}</label>
          {applying && <span className="applying" role="status"><i className="spinner" aria-hidden /> Applying…</span>}
          <span className="simple-value">{[0, 25, 50, 75, 100].includes(Math.round(pos)) ? sl.steps[Math.round(pos) / 25] : Math.round(pos)}</span>
        </div>
        <AmountSlider
          id={`simple-${sl.id}`}
          label={sl.label}
          value={pos}
          marks={sl.steps}
          auto={typeof autoAt === 'number' ? autoAt : undefined}
          onChange={(v) => {
            changeAll(applySimple(sl.id, v, settings))
            setLastChanged(stageDef.key)
          }}
        />
        <p className="plugin-description">{sl.hint}</p>
        {g && matches && (
          <p className="auto-reason">
            <span className={`chip chip-${g.confidence}`}>{CONFIDENCE_LABEL[g.confidence]}</span> {g.reason}
          </p>
        )}
      </li>
    )
  }

  const renderStage = (def: StageDef) => {
    const value = settings[def.key]
    const g = suggestionFor(def)
    const autoAt = g !== undefined ? suggestedValue(def, g) : undefined
    const matches = g !== undefined && sameValue(value, autoAt)
    const baked = BAKED.includes(def.key)
    const preparing = baked && baking && (def.key === 'dereverb' ? settings.dereverb > 0 : settings.declip)
    const meter =
      def.key === 'leveling' ? host?.leveler : def.key === 'compressor' ? host?.comp : def.key === 'deess' ? host?.deess : def.key === 'pregain' ? host?.limiter : undefined
    return (
      <li key={def.key} className="plugin-row">
        <div className="plugin-header">
          <span className="plugin-label">{def.label}</span>
          {preparing && <span className="applying" role="status" title="This is prepared in the background, then you hear it"><i className="spinner" aria-hidden /> Preparing… you’ll hear it shortly</span>}
          {!baked && host?.applying && lastChanged === def.key && <span className="applying" role="status"><i className="spinner" aria-hidden /> Applying…</span>}
          {meter !== undefined && Math.abs(meter) > 0.05 && (
            <span className="gr-readout" title="What this stage is doing right now">{meter > 0 && def.key !== 'leveling' ? '−' : meter > 0 ? '+' : ''}{Math.abs(meter).toFixed(1)} dB</span>
          )}
          {def.kind === 'gain' ? (
            <span className="simple-value">{(value as number) > 0 ? `+${(value as number).toFixed(1)} dB` : 'Off'}</span>
          ) : def.kind === 'amount' ? (
            <span className="simple-value">{def.unit ? def.unit(value as number) : amountLabel(value as number)}</span>
          ) : (
            <div className="segmented" role="group" aria-label={def.label}>
              {[{ value: false, label: 'Off' }, { value: true, label: 'On' }].map((o) => (
                <button key={String(o.value)} type="button" className={value === o.value ? 'active' : ''} onClick={() => change(def.key, o.value as never)}>
                  {o.label}
                </button>
              ))}
            </div>
          )}
        </div>
        {def.kind === 'gain' && (
          <div className="amount-slider">
            <div className="amount-track">
            <input
                id={`stage-${def.key}`}
                className="gain-slider"
                type="range"
                min={0}
                max={12}
                step={0.5}
                value={value as number}
                aria-label={def.label}
                aria-valuetext={(value as number) > 0 ? `plus ${value} decibels` : 'off'}
                onChange={(e) => change(def.key, Number(e.target.value) as never)}
              />
            </div>
            <div className="amount-ticks" aria-hidden>
              {[0, 3, 6, 9, 12].map((d) => (
                <button key={d} type="button" tabIndex={-1} onClick={() => change(def.key, d as never)}>{d === 0 ? 'Off' : `+${d}`}</button>
              ))}
            </div>
            <p className="gr-peak" role="status">
              Limiter reduction at the loudest peaks so far: <strong>{limiterPeak > 0.05 ? `−${limiterPeak.toFixed(1)} dB` : 'none yet'}</strong>
              {limiterPeak > 0.05 && <button type="button" onClick={() => setLimiterPeak(0)}>Reset</button>}
            </p>
          </div>
        )}
        {def.kind === 'amount' && (
          <AmountSlider
            id={`stage-${def.key}`}
            label={def.label}
            value={value as number}
            marks={def.marks ?? MARKS.map((m) => m.label)}
            auto={typeof autoAt === 'number' ? autoAt : undefined}
            onChange={(v) => change(def.key, v as never)}
          />
        )}
        <p className="plugin-description">
          {def.description}
        </p>
        {g && matches && (
          <p className="auto-reason">
            <span className={`chip chip-${g.confidence}`}>{CONFIDENCE_LABEL[g.confidence]}</span> {g.reason}
          </p>
        )}
      </li>
    )
  }

  // Everything still being worked on in the background, for the indicator beside "Start again".
  const labelOf = (key: keyof Settings) => STAGES.find((d) => d.key === key)?.label ?? String(key)
  const activity: string[] = []
  if (ready) {
    if (analysisState === 'running') activity.push('Auto analysis')
    if (baking && settings.dereverb > 0) activity.push(labelOf('dereverb'))
    if (baking && settings.declip) activity.push(labelOf('declip'))
    if (host?.applying && lastChanged && !(baking && (lastChanged === 'dereverb' || lastChanged === 'declip'))) activity.push(labelOf(lastChanged))
  }

  const keptSeconds = region.end - region.start
  const lufs = host && host.shortTerm > -60 ? host.shortTerm : null

  // While an export is showing, the studio is hidden but mounted; the list still shows where you are, and
  // choosing an earlier step brings the studio back.
  // Clean up earns its green tick once you have looked at it with Auto's answer in place and moved on.
  const [cleanSeen, setCleanSeen] = useState(false)
  useEffect(() => {
    if (tab === 'clean' && trimSaved && analysisState === 'done') setCleanSeen(true)
  }, [tab, trimSaved, analysisState])
  const go = (t: 'trim' | 'clean') => {
    setTab(t)
    if (paused) onReturn()
  }
  const railItems: RailItem[] = [
    { id: 'open', n: 1, title: 'Open recording', sub: source.name, state: 'done' },
    { id: 'trim', n: 2, title: 'Top and tail', sub: 'Set the start and end', state: trimSaved ? 'done' : !paused && tab === 'trim' ? 'on' : 'todo', current: !paused && tab === 'trim', onClick: () => go('trim') },
    { id: 'clean', n: 3, title: 'Clean up', sub: 'Auto, then your changes', state: cleanSeen ? 'done' : !paused && tab === 'clean' ? 'on' : 'todo', current: !paused && tab === 'clean', disabled: !trimSaved && !tourOpen, hint: trimSaved ? undefined : 'Save the trim first', onClick: () => go('clean') },
    { id: 'export', n: 4, title: 'Export…', sub: 'MP3 or WAV', state: paused ? 'on' : 'todo', current: paused, tour: 'export', disabled: paused || !ready || (!trimSaved && !tourOpen) || keptSeconds <= 0 || waitingForAuto, hint: trimSaved ? undefined : 'Save the trim first', onClick: requestExport },
  ]
  const railFoot = (
    <>
      {confirmRestart ? (
        <span className="restart-confirm">
          Discard these settings and start again?{' '}
          <button type="button" onClick={onStartAgain}>Yes, start again</button>{' '}
          <button type="button" onClick={() => setConfirmRestart(false)}>Keep working</button>
        </span>
      ) : (
        <button type="button" onClick={() => setConfirmRestart(true)}>Start again</button>
      )}
      <button type="button" className="link-button" onClick={startTour}>Take the tour</button>
    </>
  )
  const railHost = typeof document !== 'undefined' ? document.getElementById('sidebar-steps') : null
  return (
    <div className="studio">
      {railHost && createPortal(<Rail items={railItems} foot={railFoot} />, railHost)}
      <div className="studio-main">
      {waitingForAuto && (
        <div className="modal-backdrop" role="presentation">
          <div className="modal-card" role="alertdialog" aria-modal="true" aria-labelledby="wait-auto-title" aria-describedby="wait-auto-body">
            <h2 id="wait-auto-title">
              <span className="applying measuring"><i className="spinner" aria-hidden /> Waiting for Auto…</span>
            </h2>
            <p id="wait-auto-body">Auto is still listening to your recording, so that the export is made with the right settings. This only takes a moment, and the export starts by itself when it’s done.</p>
            <div className="modal-actions">
              <button type="button" onClick={() => setWaitingForAuto(false)}>Back to the studio</button>
            </div>
          </div>
        </div>
      )}
      {tourOpen && <Tour steps={tourSteps} onClose={endTour} startAt={import.meta.env.DEV ? Number(new URLSearchParams(window.location.search).get('tourstep') ?? 0) || 0 : 0} />}
      {error && <p className="upload-error">{error}</p>}
      {blockedNote && <div className="toast" role="status"><i className="spinner" aria-hidden /> Loudness processing… you can play in a moment</div>}

      <div className="transport" data-tour="transport">
        <button type="button" className={`primary transport-play ${!canPlay && !playing ? 'not-ready' : ''} ${justReady ? 'just-ready' : ''}`} aria-disabled={!canPlay && !playing} disabled={!ready && !playing} title={!canPlay && !playing ? 'Loudness processing' : undefined} onClick={togglePlay} aria-label={playing ? 'Pause' : 'Play'}>
          {playing ? 'Pause' : 'Play'}
        </button>
        {ready && solvedGain === null && !playing && (
          <span className="applying measuring" role="status"><i className="spinner" aria-hidden /> Measuring loudness… you can play in a moment</span>
        )}
        <div className="segmented" role="group" aria-label="What you hear">
          <button type="button" className={!original ? 'active' : ''} disabled={!ready} onClick={() => setOriginal(false)}>Cleaned</button>
          <button type="button" className={original ? 'active' : ''} disabled={!ready} onClick={() => setOriginal(true)}>Original</button>
        </div>
        <label className="loop-toggle"><input type="checkbox" checked={loop} onChange={(e) => setLoop(e.target.checked)} /> Loop</label>
        {host?.applying && <span className="applying" role="status"><i className="spinner" aria-hidden /> Applying changes…</span>}
        <span className="transport-time">{formatTime(host?.position ?? 0)} <span className="trim-total">/ {formatTime(source.duration)}</span></span>
        <div className="out-meter" title="Output level (peak) and loudness">
          <div className="out-meter-bar"><div className="out-meter-fill" style={{ width: meterPct(host?.peakDb ?? -120) }} /></div>
          <span title={solvedGain === null ? 'Working out the loudness level…' : `Gain set to reach the loudness target: ${solvedGain.toFixed(1)} dB`}>{lufs !== null ? `${lufs.toFixed(1)} LUFS` : '— LUFS'}{solvedGain === null ? ' …' : ''}</span>
        </div>
        {activity.length > 0 && (
          <span className="applying activity" role="status" aria-live="polite" title="These are still being worked out in the background; you'll hear each one shortly">
            <i className="spinner" aria-hidden /> <strong>Processing…</strong> <span className="activity-list">{activity.join(' · ')}</span>
          </span>
        )}
      </div>

      {loading && (
        <div className="progress">
          <div className="progress-bar"><div className="progress-fill" style={{ width: `${Math.max(4, loading.fraction * 100)}%` }} /></div>
          <p className="trim-hint">{loading.label} Long recordings take a moment the first time.</p>
        </div>
      )}

      <div ref={waveRef} className="trim-waveform" data-tour="waveform" />
      <div className="trim-controls">
        <div className="trim-times">
          <span>Start: {formatTime(region.start)}</span>
          <span>End: {formatTime(region.end)}</span>
          <span>Length: {formatTime(keptSeconds)}</span>
        </div>
      </div>

      {tab === 'trim' && (
        <>
      {(
        <div className="trim-detail-grid">
          <TrimDetail
            label="Start" side="start" time={region.start} limit={region.end} duration={source.duration} fine={source.fine}
            onChange={(t) => setMarker('start', t)} onHear={() => hearFrom(region.start)} hearDisabled={!canPlay} justReady={justReady} onUsePlayhead={() => setMarker('start', host?.position ?? 0)}
          />
          <TrimDetail
            label="End" side="end" time={region.end} limit={region.start} duration={source.duration} fine={source.fine}
            onChange={(t) => setMarker('end', t)} onHear={() => hearFrom(Math.max(region.start, region.end - 6))} hearDisabled={!canPlay} justReady={justReady} onUsePlayhead={() => setMarker('end', host?.position ?? source.duration)}
          />
        </div>
      )}


          <div className="trim-save" data-tour="save-trim">
            <span><strong>Start just before the first word, end just after the last.</strong> Saving moves on to Clean up.</span>
            <button type="button" className="primary" disabled={!ready || keptSeconds <= 0} onClick={() => { setTrimSaved(true); setTab('clean') }}>Save trim and continue</button>
          </div>
          <p className="trim-hint">
            Space plays and pauses · click the waveform to move the playhead · <kbd>I</kbd>/<kbd>O</kbd> set the start/end at the playhead.
          </p>
        </>
      )}

      {tab === 'clean' && (
        <>
          <div className="trim-save saved">
            <span>Kept <strong>{formatTime(region.start)}</strong> to <strong>{formatTime(region.end)}</strong> ({formatTime(keptSeconds)}).</span>
            <button type="button" onClick={() => setTab('trim')}>Edit trim</button>
          </div>
      <div className="hymn-board" role="group" aria-label="Today's settings, 0 to 100">
        {([
          ['Cleaner background', settings.noise],
          ['Room echo', settings.dereverb],
          ['Voice clarity', settings.deess],
          ['Even volume', settings.compressor],
          ['Loudness', settings.loudness],
        ] as const).map(([label, value]) => (
          <div key={label} className={`hymn-tile ${trimSaved && analysisState !== 'running' ? '' : 'waiting'}`}>
            <b>{trimSaved && analysisState !== 'running' ? String(Math.round(value)).padStart(3, '0') : '---'}</b>
            <span>{label}</span>
          </div>
        ))}
      </div>

          <p className="trim-hint"><kbd>A</kbd> switches original/cleaned · <kbd>L</kbd> loops. Change any setting below while it plays.</p>
      <div className={trimSaved ? '' : 'locked'} aria-disabled={!trimSaved}>
      <div className={`auto-banner ${custom ? 'custom' : 'auto'}`} data-tour="auto">
        {!trimSaved && <p><strong>Auto</strong> · Starts as soon as you save the trim.</p>}
        {analysisState === 'running' && <p><strong>Auto</strong> · Listening to your recording…</p>}
        {analysisState === 'done' && !custom && <p><strong>Auto</strong> · Chosen by analysing your recording ({formatTime(analysis?.analyzedFrom ?? 0)} to {formatTime(analysis?.analyzedTo ?? 0)}). Change anything you like.</p>}
        {analysisState === 'done' && custom && <p><strong>Custom settings</strong> · You’ve changed something from Auto.</p>}
        {analysisState === 'failed' && <p><strong>Standard settings</strong> · Couldn’t analyse this recording, so the standard settings are shown.</p>}
        <div className="auto-actions">
          <button type="button" disabled={analysisState === 'running' || !custom} onClick={() => { setSettings(suggested ?? STANDARD); setTouched(false) }}>Reset to Auto</button>
          <button type="button"  onClick={() => { setSettings(STANDARD); setTouched(true) }}>Standard settings</button>
        </div>
      </div>

      <div className="view-switch segmented" role="group" aria-label="Settings view">
        <button type="button" className={view === 'simple' ? 'active' : ''} onClick={() => chooseView('simple')}>Simple</button>
        <button type="button" className={view === 'advanced' ? 'active' : ''} onClick={() => chooseView('advanced')}>Advanced</button>
      </div>
      {view === 'simple' ? (
        <>
          <ul className="plugin-list" data-tour="sliders">{SIMPLE_SLIDERS.map(renderSimple)}</ul>
          <p className="trim-hint">Auto has also set repair clipping, room echo and tone for this recording{settings.pregain > 0 ? `, and you’ve added +${settings.pregain.toFixed(1)} dB of limiter pre-gain` : ''}. Switch to Advanced to see or change every setting.</p>
        </>
      ) : (
        <ul className="plugin-list" data-tour="sliders">{STAGES.map(renderStage)}</ul>
      )}
      </div>

          <div className="clean-actions">
            <button type="button" className="primary" disabled={!ready || !trimSaved || keptSeconds <= 0 || waitingForAuto} onClick={requestExport}>Export…</button>
          </div>
        </>
      )}
      </div>
    </div>
  )
}
