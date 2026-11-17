// The clean-up chain as the UI sees it: every stage is a slider from 0 (off) to 100 (max).
//
// The old Less / Normal / More presets are fixed points on the scale (25 / 50 / 75), so a recording that
// used to be Normal sounds the same at 50; Max (100) goes past the old More. See native/src/amount.h for
// the macro curve behind each slider (what each position does to the stage's underlying parameters).
export interface Settings {
  declip: boolean
  leveling: number
  noise: number
  dereverb: number
  tone: number
  deess: number
  compressor: number
  loudness: number // the target loudness; 0 leaves the level alone
  pregain: number // dB of extra gain into the final limiter, 0-12, after the loudness is set
}

/** The named positions every amount slider passes through. */
export const MARKS: { value: number; label: string }[] = [
  { value: 0, label: 'Off' },
  { value: 25, label: 'Less' },
  { value: 50, label: 'Normal' },
  { value: 75, label: 'More' },
  { value: 100, label: 'Max' },
]

/** "Off", "Normal", or "62" - the name when the slider sits on a named position, else the number. */
export function amountLabel(v: number): string {
  const mark = MARKS.find((m) => m.value === Math.round(v))
  return mark ? mark.label : String(Math.round(v))
}

/** The fixed defaults ("Standard settings"), used when Auto can't or shouldn't decide. */
export const STANDARD: Settings = {
  declip: true,
  leveling: 50,
  noise: 50,
  dereverb: 0,
  tone: 50,
  deess: 50,
  compressor: 50,
  loudness: 50,
  pregain: 0,
}

export interface Suggestion {
  stage: string
  setting: string
  amount?: number // the suggested slider position, 0-100, for stages that have one (-1 or absent otherwise)
  reason: string
  confidence: 'high' | 'medium' | 'low'
}

export interface Measurements {
  seconds: number
  snrDb: number
  noiseFloorDb: number
  lufs: number
  loudnessRangeLu: number
  [key: string]: number
}

export interface Analysis {
  analyzedFrom: number
  analyzedTo: number
  measurements: Measurements
  suggestions: Suggestion[]
}

const WORD_AMOUNT: Record<string, number> = { off: 0, less: 25, normal: 50, more: 75, max: 100 }

/** The slider position a suggestion stands for, or undefined if it isn't a value for an amount stage. */
function suggestedAmount(g: Suggestion): number | undefined {
  if (typeof g.amount === 'number' && g.amount >= 0) return g.amount
  return WORD_AMOUNT[g.setting]
}

/** Applies the engine's suggestions on top of the standard settings. */
export function settingsFromSuggestions(suggestions: Suggestion[]): Settings {
  const s: Settings = { ...STANDARD }
  for (const g of suggestions) {
    switch (g.stage) {
      case 'noise':
      case 'dereverb':
      case 'deess':
      case 'compressor': {
        const v = suggestedAmount(g)
        if (v !== undefined) s[g.stage] = v
        break
      }
      case 'leveling': {
        // Older engines said on/off; newer ones suggest how much.
        const v = typeof g.amount === 'number' && g.amount >= 0 ? g.amount : g.setting === 'off' ? 0 : g.setting === 'on' ? 50 : suggestedAmount(g)
        if (v !== undefined) s.leveling = v
        break
      }
      case 'declip':
        s.declip = g.setting === 'on'
        break
    }
  }
  return s
}

/** What a suggestion means for a stage's current-value type, or undefined if it isn't a value for this stage. */
export function suggestedValue(def: { key: keyof Settings; kind: 'amount' | 'toggle' | 'gain' }, g: Suggestion): Settings[keyof Settings] | undefined {
  if (def.kind === 'toggle') return g.setting === 'on' ? true : g.setting === 'off' ? false : undefined
  if (def.key === 'leveling') return g.setting === 'off' ? 0 : g.setting === 'on' ? 50 : suggestedAmount(g)
  return suggestedAmount(g)
}

/** Two values count as the same setting if they are within a point or two (sliders are imprecise). */
export function sameValue(a: Settings[keyof Settings], b: Settings[keyof Settings] | undefined): boolean {
  if (typeof a === 'number' && typeof b === 'number') return Math.abs(a - b) <= 1
  return a === b
}

export function sameSettings(a: Settings, b: Settings): boolean {
  return (Object.keys(a) as (keyof Settings)[]).every((k) => sameValue(a[k], b[k]))
}

/** The loudness (LUFS) a slider position asks for. Keep in step with `targetLufs` in native/src/amount.h. */
export function loudnessTarget(v: number): number {
  const knots = [-18, -18, -14, -12.5, -11] // at 0 (unused: off), 25, 50, 75, 100
  const p = Math.min(100, Math.max(0, v)) / 25
  const i = Math.min(3, Math.floor(p))
  return knots[i] + (knots[i + 1] - knots[i]) * (p - i)
}

export interface StageDef {
  key: keyof Settings
  stage: string // the suggestion stage this maps to
  label: string
  description: string
  kind: 'amount' | 'toggle' | 'gain'
  /** Names for positions 0, 25, 50, 75, 100 if they aren't the usual Off / Less / Normal / More / Max. */
  marks?: string[]
  /** What a position means as a number, shown beside the name (e.g. the loudness in LUFS). */
  unit?: (v: number) => string
}

// In processing order (see native/src/pipeline.cpp).
export const STAGES: StageDef[] = [
  { key: 'declip', stage: 'declip', kind: 'toggle', label: 'Repair clipping', description: 'Smooths out moments that were clipped (distorted) by the recorder or PA. Does nothing if there is none.' },
  { key: 'leveling', stage: 'leveling', kind: 'amount', label: 'Even out volume', description: 'Rides the level so quiet and loud stretches come out closer together, like a person steadily working a fader. Higher follows the speaker more closely.' },
  { key: 'noise', stage: 'noise', kind: 'amount', label: 'Remove background noise', description: 'Removes hiss, hum and room noise between and under the words. Max takes out as much as it can; very high settings can make the voice sound processed.' },
  { key: 'dereverb', stage: 'dereverb', kind: 'amount', label: 'Reduce room echo', description: 'Dries up a boomy or echoey room. The first time you raise it, the engine prepares it in the background for a minute or so; after that the slider is instant. Very high settings can thin the voice slightly.' },
  { key: 'tone', stage: 'tone', kind: 'amount', label: 'Tone', description: 'Cuts boom and boxiness and tames harshness for a cleaner speaking voice. Normal is the Studio match, the tone shaping from the owner’s Pro Tools sessions; lower is gentler, higher goes further.' },
  { key: 'deess', stage: 'deess', kind: 'amount', label: 'Reduce sharp S sounds', description: 'Softens harsh “s” and “t” sounds without dulling the rest of the voice. Very high settings can start to lisp.' },
  { key: 'compressor', stage: 'compressor', kind: 'amount', label: 'Compression', description: 'Tightens up the volume swings that are left, for a more consistent level.' },
  {
    key: 'loudness',
    stage: 'loudness',
    kind: 'amount',
    label: 'Loudness',
    description: 'Sets how loud the finished recording is. Normal is the streaming standard (−14 LUFS, what YouTube and Spotify play at); lower keeps more of the natural dynamics, higher is louder. Whatever this is set to, a final true-peak limiter always runs last and holds the peaks at −1 dB, the ceiling YouTube and Spotify ask for. Off leaves the level as it is and only applies that limiter.',
    marks: ['Off', 'Quieter', 'Standard', 'Louder', 'Loudest'],
    unit: (v) => (v <= 0 ? 'level unchanged · peaks −1 dB' : `${loudnessTarget(v).toFixed(1).replace('-', '−')} LUFS`),
  },
  {
    key: 'pregain',
    stage: 'pregain',
    kind: 'gain',
    label: 'Limiter pre-gain',
    description: 'Extra gain pushed into the final limiter after the loudness is set, up to +12 dB, so the limiter works on the peaks. The output ends up louder than the target by about this much, and the peaks stay held at −1 dB. Watch the reduction readout: around 4 dB at the highest peaks is a good place to aim.',
  },
]

// ---- Simple view: three sliders that each drive a group of settings ----
export interface SimpleSlider {
  id: 'clean' | 'clarity' | 'even'
  label: string
  hint: string
  steps: string[] // what the five named positions (0, 25, 50, 75, 100) are called here
  stage: string // the stage whose Auto suggestion explains this slider
}

export const SIMPLE_SLIDERS: SimpleSlider[] = [
  { id: 'clean', label: 'Cleaner background', hint: 'How much hiss, hum and room noise to remove.', steps: ['Off', 'Light', 'Normal', 'Strong', 'Max'], stage: 'noise' },
  { id: 'clarity', label: 'Voice clarity', hint: 'Tone shaping and softening of harsh “s” sounds.', steps: ['Natural', 'Soft', 'Balanced', 'Crisp', 'Max'], stage: 'deess' },
  { id: 'even', label: 'Even volume', hint: 'How much to even out quiet and loud stretches.', steps: ['Off', 'Light', 'Normal', 'Strong', 'Max'], stage: 'compressor' },
]

/** Where a simple slider sits (0-100) for the current settings. */
export function simplePosition(id: SimpleSlider['id'], s: Settings): number {
  if (id === 'clean') return s.noise
  if (id === 'clarity') return s.deess
  return s.compressor
}

/** The settings a slider position stands for, applied on top of the current ones. */
export function applySimple(id: SimpleSlider['id'], position: number, s: Settings): Settings {
  if (id === 'clean') return { ...s, noise: position }
  if (id === 'clarity') return { ...s, tone: position, deess: position }
  return { ...s, compressor: position, leveling: position }
}
