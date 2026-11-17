import { useCallback, useEffect, useLayoutEffect, useRef, useState } from 'react'

// A guided tour: dims the page, highlights one part of it at a time and explains it. Steps point at
// elements marked with data-tour="name". A step can run an action first (e.g. open a hidden panel).
export interface TourStep {
  target: string // the data-tour name of the element to highlight
  title: string
  body: React.ReactNode
  /** Runs when the step is shown, before the element is measured. */
  before?: () => void
}

interface Rect {
  top: number
  left: number
  width: number
  height: number
}

const PAD = 6 // breathing room around the highlighted element
const CARD_GAP = 14

export default function Tour({ steps, onClose, startAt = 0 }: { steps: TourStep[]; onClose: () => void; startAt?: number }) {
  const [index, setIndex] = useState(Math.min(Math.max(0, startAt), steps.length - 1))
  const [rect, setRect] = useState<Rect | null>(null)
  const [cardSize, setCardSize] = useState({ width: 380, height: 220 })
  const cardRef = useRef<HTMLDivElement>(null)
  const nextRef = useRef<HTMLButtonElement>(null)
  const step = steps[index]
  const last = index === steps.length - 1

  const measure = useCallback(() => {
    const el = document.querySelector<HTMLElement>(`[data-tour="${step.target}"]`)
    if (!el) {
      setRect(null)
      return
    }
    const r = el.getBoundingClientRect()
    setRect({ top: r.top, left: r.left, width: r.width, height: r.height })
  }, [step.target])

  // Show the step: run its action, bring the element into view, then measure it (again once any
  // scrolling or panel opening has settled).
  useEffect(() => {
    step.before?.()
    const el = () => document.querySelector<HTMLElement>(`[data-tour="${step.target}"]`)
    const t1 = setTimeout(() => {
      el()?.scrollIntoView({ block: 'start', behavior: window.matchMedia('(prefers-reduced-motion: reduce)').matches ? 'auto' : 'smooth' })
      measure()
    }, 120)
    const t2 = setTimeout(measure, 600)
    return () => {
      clearTimeout(t1)
      clearTimeout(t2)
    }
  }, [index]) // eslint-disable-line react-hooks/exhaustive-deps

  useEffect(() => {
    window.addEventListener('resize', measure)
    window.addEventListener('scroll', measure, true)
    return () => {
      window.removeEventListener('resize', measure)
      window.removeEventListener('scroll', measure, true)
    }
  }, [measure])

  useLayoutEffect(() => {
    const c = cardRef.current
    if (c) setCardSize({ width: c.offsetWidth, height: c.offsetHeight })
  }, [index, rect])

  useEffect(() => {
    nextRef.current?.focus()
  }, [index])

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') onClose()
      else if (e.key === 'ArrowRight') setIndex((i) => Math.min(steps.length - 1, i + 1))
      else if (e.key === 'ArrowLeft') setIndex((i) => Math.max(0, i - 1))
      else return
      e.preventDefault()
      e.stopPropagation()
    }
    window.addEventListener('keydown', onKey, true)
    return () => window.removeEventListener('keydown', onKey, true)
  }, [onClose, steps.length])

  // Place the card where it covers the highlighted part the least: below, above, to the right or left, or
  // pinned to the bottom of the window.
  const vw = window.innerWidth
  const vh = window.innerHeight
  const { width: cw, height: ch } = cardSize
  const clampX = (x: number) => Math.min(Math.max(16, x), Math.max(16, vw - cw - 16))
  const clampY = (y: number) => Math.min(Math.max(12, y), Math.max(12, vh - ch - 12))
  let cardTop = clampY(vh - ch - 16)
  let cardLeft = clampX((vw - cw) / 2)
  if (rect) {
    const spot = { l: rect.left - PAD, t: rect.top - PAD, r: rect.left + rect.width + PAD, b: rect.top + rect.height + PAD }
    const centreX = rect.left + rect.width / 2 - cw / 2
    const centreY = rect.top + rect.height / 2 - ch / 2
    const candidates = [
      { top: clampY(spot.b + CARD_GAP), left: clampX(centreX) }, // below
      { top: clampY(centreY), left: clampX(spot.r + CARD_GAP) }, // right
      { top: clampY(centreY), left: clampX(spot.l - CARD_GAP - cw) }, // left
      { top: clampY(spot.t - CARD_GAP - ch), left: clampX(centreX) }, // above
      { top: clampY(vh - ch - 16), left: clampX(centreX) }, // bottom of the window
    ]
    const overlap = (c: { top: number; left: number }) =>
      Math.max(0, Math.min(c.left + cw, spot.r) - Math.max(c.left, spot.l)) * Math.max(0, Math.min(c.top + ch, spot.b) - Math.max(c.top, spot.t))
    let best = candidates[0]
    for (const c of candidates) if (overlap(c) < overlap(best)) best = c
    cardTop = best.top
    cardLeft = best.left
  }

  return (
    <div className="tour" role="dialog" aria-modal="true" aria-label="Guided tour">
      <div className="tour-blocker" onClick={(e) => e.stopPropagation()} />
      {rect && <div className="tour-spot" style={{ top: rect.top - PAD, left: rect.left - PAD, width: rect.width + PAD * 2, height: rect.height + PAD * 2 }} />}
      {!rect && <div className="tour-dim" />}
      <div ref={cardRef} className="tour-card" style={{ top: cardTop, left: cardLeft }}>
        <p className="tour-count">
          Step {index + 1} of {steps.length}
        </p>
        <h2>{step.title}</h2>
        <div className="tour-body">{step.body}</div>
        <div className="tour-actions">
          <button type="button" className="tour-skip" onClick={onClose}>
            {last ? 'Close' : 'Skip tour'}
          </button>
          <span className="tour-nav">
            <button type="button" disabled={index === 0} onClick={() => setIndex(index - 1)}>
              Back
            </button>
            <button ref={nextRef} type="button" className="primary" onClick={() => (last ? onClose() : setIndex(index + 1))}>
              {last ? 'Got it' : 'Next'}
            </button>
          </span>
        </div>
      </div>
    </div>
  )
}
