// Development-only: a fake engine so the UI can be rendered and clicked through in a plain
// browser (npm run dev, then open /?mock=upload|trim|cleanup|processing|download).
import { mockConvertFileSrc, mockIPC } from '@tauri-apps/api/mocks'
import { emit } from '@tauri-apps/api/event'

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms))

const SUGGESTIONS = [
  { stage: 'noise', setting: 'normal', reason: 'Speech sits 44 dB above the background noise (-66 dBFS).', confidence: 'medium' },
  { stage: 'declip', setting: 'off', reason: 'No clipping detected.', confidence: 'high' },
  { stage: 'leveling', setting: 'on', reason: 'Level is fairly steady (2.7 dB drift); leveling will make only small corrections.', confidence: 'medium' },
  { stage: 'dereverb', setting: 'off', reason: 'The room is fairly dry (echo tail -31 dB below the speech).', confidence: 'low' },
  { stage: 'deess', setting: 'normal', reason: 'High-frequency (4-9 kHz) energy is -10 dB relative to the voice body.', confidence: 'low' },
  { stage: 'compressor', setting: 'more', reason: 'Loudness varies over a range of 14 LU.', confidence: 'medium' },
]

const STEPS = ['Resampling to 48 kHz…', 'Setting starting volume…', 'Removing rumble…', 'Evening out volume…', 'Removing hum…', 'Removing background noise…', 'Shaping tone…', 'Softening sharp S sounds…', 'Compressing…', 'Reducing encoder hiss…', 'Matching streaming loudness…']

export function installMocks() {
  mockConvertFileSrc('macos')
  mockIPC(
    (cmd, payload) => {
      const p = (payload ?? {}) as { jobId?: string; args?: string[]; name?: string }
      if (cmd === 'scratch_path') return `/tmp/sermon-polish/${p.name}`
      if (cmd === 'plugin:dialog|save') return '/Users/demo/Sermons/2026.10.25 - Wash your wife.mp3'
      if (cmd === 'plugin:dialog|open') return '/Users/demo/Sermons/sermon.mp3'
      if (cmd === 'run_sidecar' && p.jobId && p.args) {
        const jobId = p.jobId
        const args = p.args
        const sidecar = (payload as { sidecar?: string }).sidecar
        const send = (kind: string, data: string) => emit('engine-event', { job_id: jobId, kind, data })
        void (async () => {
          await sleep(150)
          if (sidecar === 'ffmpeg') {
            if (args.includes('-progress')) {
              // a conversion: report progress slowly enough to see it
              for (let i = 1; i <= 20; i++) {
                await send('line', `out_time_us=${i * 138_000_000}`)
                await sleep(450)
              }
              await send('line', 'progress=end')
              await send('exit', '0')
            } else {
              // an inspection: ffmpeg prints the streams then exits non-zero (no output file given)
              await send('stderr', '  Duration: 00:46:02.60, start: 0.000000, bitrate: 2210 kb/s\n  Stream #0:0(und): Video: h264 (High), yuv420p, 1920x1080, 25 fps\n  Stream #0:1(und): Audio: aac (LC), 48000 Hz, stereo, fltp, 160 kb/s\n')
              await send('exit', '1')
            }
            return
          }
          if (args.includes('--analyze')) {
            await sleep(500)
            await send('line', JSON.stringify({ event: 'analysis', analyzedFrom: 0, analyzedTo: 2762.6, measurements: { seconds: 2762, snrDb: 44, noiseFloorDb: -66, lufs: -27.4, loudnessRangeLu: 13.7 }, suggestions: SUGGESTIONS }))
          } else {
            const slow = args.includes('--trim') && !args.includes('--wav16') && !args.includes('--no-chain')
            for (let i = 0; i < STEPS.length; i++) {
              await send('line', JSON.stringify({ event: 'progress', label: STEPS[i], step: i + 1, total: STEPS.length }))
              await sleep(slow ? 700 : 80)
            }
            await send('line', JSON.stringify({ event: 'result', output: { seconds: 2650.3, lufs: -14.1, truePeakDb: -1 } }))
          }
          await send('exit', '0')
        })()
        return null
      }
      return null
    },
    { shouldMockEvents: true },
  )
}
