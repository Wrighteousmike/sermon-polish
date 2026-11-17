// Turns "any file that contains audio" into a WAV the engine can read. The engine itself reads
// WAV and MP3 natively; everything else (M4A/AAC, FLAC, OGG/Opus, WMA, AIFF ... and video files
// such as MP4/MOV/MKV/AVI/WebM, whose audio track is pulled out) goes through the bundled ffmpeg.
import { runFfmpeg, scratchPath, type RawRun } from './engine'

export interface MediaInfo {
  hasAudio: boolean
  channels: number
  sampleRate: number
  durationSeconds: number
  codec: string
  hasVideo: boolean
}

/** Channel layouts as ffmpeg names them in its stream description. */
function channelCount(layout: string): number {
  const l = layout.trim().toLowerCase()
  const n = l.match(/^(\d+) channels/)
  if (n) return Number(n[1])
  if (l === 'mono') return 1
  if (l === 'stereo') return 2
  const m = l.match(/^(\d+)\.(\d)/) // 2.1, 5.1, 7.1 ...
  if (m) return Number(m[1]) + Number(m[2])
  return 2
}

/** Parses ffmpeg's `-i` banner (written to stderr) into what we need to know about the file. */
export function parseMediaInfo(text: string): MediaInfo {
  const dur = text.match(/Duration:\s*(\d+):(\d+):(\d+(?:\.\d+)?)/)
  const audio = text.match(/Stream #\d+:\d+[^\n]*: Audio: ([^,\n(]+)[^,\n]*, (\d+) Hz, ([^,\n]+)/)
  return {
    hasAudio: audio !== null,
    channels: audio ? channelCount(audio[3]) : 0,
    sampleRate: audio ? Number(audio[2]) : 0,
    durationSeconds: dur ? Number(dur[1]) * 3600 + Number(dur[2]) * 60 + Number(dur[3]) : 0,
    codec: audio ? audio[1].trim() : '',
    hasVideo: /Stream #\d+:\d+[^\n]*: Video:/.test(text),
  }
}

/** Asks ffmpeg what is in the file (it exits with an error after printing, which is expected). */
export async function inspectMedia(path: string): Promise<MediaInfo> {
  const { text } = await runFfmpeg(['-hide_banner', '-nostdin', '-i', path]).done
  return parseMediaInfo(text)
}

export interface Conversion {
  job: RawRun
  outPath: string
  /** Resolves when the WAV is written. Rejects with a readable message on failure. */
  done: Promise<void>
}

/**
 * Extracts the first audio track as a lossless 24-bit WAV in the app's scratch folder. Files with
 * more than two channels (surround video) are mixed down to stereo. `onProgress` gets 0..1.
 */
export async function convertToWav(path: string, info: MediaInfo, onProgress?: (fraction: number) => void): Promise<Conversion> {
  const outPath = await scratchPath(`converted-${Date.now()}.wav`)
  const args = [
    '-hide_banner', '-nostdin', '-y', '-i', path,
    '-vn', '-sn', '-dn', '-map', '0:a:0',
    ...(info.channels > 2 ? ['-ac', '2'] : []),
    '-c:a', 'pcm_s24le',
    '-progress', 'pipe:1', '-nostats',
    outPath,
  ]
  const job = runFfmpeg(args, (line) => {
    const m = line.match(/^out_time_(?:us|ms)=(\d+)/) // ffmpeg reports microseconds under both names
    if (m && info.durationSeconds > 0) onProgress?.(Math.min(1, Number(m[1]) / 1e6 / info.durationSeconds))
  })
  const done = job.done.then(({ code, text }) => {
    if (code !== 0) {
      const reason = text.split('\n').filter((l) => /error|invalid|no such|not found|unsupported/i.test(l)).pop()
      throw new Error(reason ? `Couldn’t read the audio in this file (${reason.trim().slice(0, 140)}).` : 'Couldn’t read the audio in this file.')
    }
  })
  return { job, outPath, done }
}

/** File types worth offering in the picker (any other file can still be tried via "All files"). */
export const MEDIA_EXTENSIONS = [
  'wav', 'mp3', 'm4a', 'aac', 'mp4', 'm4v', 'mov', 'mkv', 'webm', 'avi', 'flac', 'ogg', 'oga', 'opus', 'wma', 'wmv',
  'aif', 'aiff', 'caf', 'amr', 'ac3', 'ts', 'mts', 'mpg', 'mpeg', 'flv', '3gp',
]

/** Formats the engine reads directly (no conversion needed). */
export const NATIVE_EXTENSIONS = ['wav', 'mp3']
