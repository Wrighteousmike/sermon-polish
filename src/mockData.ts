// Development-only fixtures for ?mock=<screen> (see mock.ts). Not used in production builds.
import type { Source } from './engine/source'

const peaks: number[] = []
for (let i = 0; i < 3000; i++) {
  const phrase = Math.abs(Math.sin(i / 41)) * (0.35 + 0.5 * Math.abs(Math.sin(i / 233)))
  peaks.push(Math.min(1, phrase * (0.6 + 0.4 * Math.abs(Math.sin(i * 1.7))) + (i % 97 < 6 ? 0.02 : 0)))
}

// Fine (1 ms) fake waveform: phrases of speech-like amplitude with pauses.
const FINE_COUNT = Math.round(2762.6 * 1000)
const fineData = new Int16Array(FINE_COUNT * 2)
for (let i = 0; i < FINE_COUNT; i++) {
  const t = i / 1000
  const phrase = Math.abs(Math.sin(t / 2.3)) * (0.35 + 0.5 * Math.abs(Math.sin(t / 11)))
  const syllable = 0.45 + 0.55 * Math.abs(Math.sin(t * 7.3))
  const amp = Math.min(1, phrase * syllable) * (0.8 + 0.2 * Math.sin(i * 12.9898))
  fineData[2 * i] = Math.round(-amp * 0.9 * 32767)
  fineData[2 * i + 1] = Math.round(amp * 32767)
}

export const mockSource: Source = {
  path: '/Users/demo/Sermons/2026.10.25 - Wash your wife (Eph 5v25-33) - D. Jones.mp3',
  name: '2026.10.25 - Wash your wife (Eph 5v25-33) - D. Jones.mp3',
  duration: 2762.6,
  sampleRate: 22050,
  channels: 1,
  peaks,
  fine: { bucketSeconds: 0.001, data: fineData },
}
