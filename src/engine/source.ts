import { convertFileSrc } from '@tauri-apps/api/core'
import { removeScratchFile, runEngine, scratchPath } from './engine'

/** 1 ms-resolution waveform of the whole recording, for the zoomed trim views. */
export interface FinePeaks {
  bucketSeconds: number
  data: Int16Array // interleaved (min, max) per bucket, value / 32767
}

/** A recording the user picked, as reported by the engine. */
export interface Source {
  path: string // what the engine reads (the original, or a converted WAV in the scratch folder)
  name: string // the original file's name, for display and for naming the saved MP3
  scratchFile?: string // set when `path` is a converted copy that should be deleted afterwards
  duration: number // seconds
  sampleRate: number
  channels: number
  peaks: number[] // one max-amplitude value per bucket, for WaveSurfer's overview
  fine: FinePeaks
}

export const fileNameOf = (path: string) => path.split(/[\\/]/).pop() ?? path

/** Reads the file's duration and waveform overview. The engine decodes it; the webview never does. */
export async function probeFile(path: string, displayName?: string): Promise<Source> {
  const stamp = Date.now()
  const peaksPath = await scratchPath(`peaks-${stamp}.json`)
  const finePath = await scratchPath(`peaks-${stamp}.bin`)
  try {
    const ev = await runEngine([path, '--probe', '--peaks', peaksPath, '--peaks-buckets', '3000', '--peaks-bin', finePath, '--json']).done
    const res = await fetch(convertFileSrc(peaksPath))
    const json = (await res.json()) as { peaks: number[] }
    const peaks: number[] = []
    for (let i = 0; i + 1 < json.peaks.length; i += 2) peaks.push(Math.max(Math.abs(json.peaks[i]), Math.abs(json.peaks[i + 1])))
    const buf = await (await fetch(convertFileSrc(finePath))).arrayBuffer()
    const header = new DataView(buf)
    if (buf.byteLength < 16 || header.getUint32(0, true) !== 0x314b5053 /* "SPK1" */) throw new Error('Could not read the waveform data.')
    const fine: FinePeaks = { bucketSeconds: header.getFloat32(12, true), data: new Int16Array(buf, 16, Math.floor((buf.byteLength - 16) / 2)) }
    return {
      path,
      name: displayName ?? fileNameOf(path),
      duration: Number(ev.seconds),
      sampleRate: Number(ev.sampleRate),
      channels: Number(ev.channels),
      peaks,
      fine,
    }
  } finally {
    void removeScratchFile(peaksPath)
    void removeScratchFile(finePath)
  }
}
