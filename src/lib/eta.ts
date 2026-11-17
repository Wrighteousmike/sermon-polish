// Time-remaining estimates for an export.
//
// How the engine lays out a render (see renderExport in native/src/rt/render.cpp): preparing the echo
// reduction (no way to know how long it takes), cleaning (progress 0 to 0.80, the bulk of the work), joining the
// pieces (0.80 to 0.85), then setting the loudness. Those last two run on one thread while the cleaning uses
// every core, so their time relative to the cleaning grows with the number of cores. Measured on a 10-core Mac:
// joining took 0.15x and the loudness 0.18x of the cleaning time.
const CLEAN_SPAN = 0.8
const JOIN_PER_CORE = 0.015
const LOUDNESS_PER_CORE = 0.018

/** "about 3 min remaining". */
export function etaText(seconds: number): string {
  if (seconds < 10) return 'a few seconds remaining'
  if (seconds < 60) return `about ${Math.max(10, Math.round(seconds / 5) * 5)} seconds remaining`
  return `about ${Math.ceil(seconds / 60)} min remaining`
}

/**
 * Only ever uses time spent on the actual work: the wait for echo reduction to be prepared isn't counted (that
 * once made the first estimate an hour too long), and nothing is shown until enough of the cleaning is done to
 * say something sensible. Returns seconds remaining, or null when there is no estimate yet.
 */
export class RenderEta {
  private label = ''
  private start = 0
  private cleaningTotal = 0
  private smoothed: number | null = null
  private anchor: { t: number; pf: number } | null = null
  private joinShare: number
  private loudnessShare: number

  constructor(cores = 8) {
    const c = Math.max(1, Math.min(32, cores))
    this.joinShare = JOIN_PER_CORE * c
    this.loudnessShare = LOUDNESS_PER_CORE * c
  }

  private show(remaining: number | null): number | null {
    if (remaining === null) return null
    this.smoothed = this.smoothed === null ? remaining : 0.75 * this.smoothed + 0.25 * remaining
    return this.smoothed
  }

  render(label: string, fraction: number, now: number): number | null {
    if (label !== this.label) {
      this.label = label
      this.start = now
      this.anchor = null
    }
    const elapsed = (now - this.start) / 1000
    if (label.startsWith('Cleaning')) {
      const pf = Math.min(1, fraction / CLEAN_SPAN)
      // The first moments are the workers starting up, so the pace is measured from a little way in, not from the start.
      if (!this.anchor) {
        if (pf >= 0.05) this.anchor = { t: now, pf }
        return null
      }
      const since = (now - this.anchor.t) / 1000
      const gained = pf - this.anchor.pf
      if (since < 4 || gained < 0.08) return null
      const toGo = (since * (1 - pf)) / gained // seconds of cleaning left, at the pace since the anchor
      this.cleaningTotal = elapsed + toGo
      return this.show(toGo + this.cleaningTotal * (this.joinShare + this.loudnessShare))
    }
    if (label.startsWith('Joining') && this.cleaningTotal > 0) {
      const pf = Math.min(1, Math.max(0, (fraction - CLEAN_SPAN) / 0.05))
      return this.show(this.cleaningTotal * (this.joinShare * (1 - pf) + this.loudnessShare))
    }
    if (label.startsWith('Setting') && this.cleaningTotal > 0) return this.show(Math.max(2, this.cleaningTotal * this.loudnessShare - elapsed))
    return null // preparing, finished, or not enough to go on
  }

  /** A simple steady job (encoding an MP3): estimated from the share done so far. */
  simple(fraction: number, now: number): number | null {
    if (!this.start) this.start = now
    const elapsed = (now - this.start) / 1000
    if (fraction < 0.1 || fraction >= 0.995 || elapsed < 3) return null
    return this.show((elapsed * (1 - fraction)) / fraction)
  }
}
