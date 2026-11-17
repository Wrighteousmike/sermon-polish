/** Sermon metadata used to build the export filename and MP3 ID3 tag. */
export interface TagFields {
  date: string
  text: string
  title: string
  speaker: string
}

const ARTIST = 'oneflock.co.za ministry tools'

// Scripture references use ":" naturally ("Ephesians 1:1-3") but ":" isn't
// filesystem-safe on Windows — the filename form uses "v" instead
// ("Ephesians 1v1-3"), matching the convention the maintainer already uses for his
// own sermon files (e.g. "Ecclesiastes 11v8-12v14").
export function toFilenameText(text: string): string {
  return text.replace(/(\d)\s*:\s*(\d)/g, '$1v$2')
}

export function fromFilenameText(text: string): string {
  return text.replace(/(\d)v(\d)/g, '$1:$2')
}

function sanitizeForFilename(name: string): string {
  // eslint-disable-next-line no-control-regex
  return name.replace(/[<>:"/\\|?*\x00-\x1f]/g, '').trim()
}

/**
 * Builds "Date - Text - Title (Speaker)", omitting any field that's empty.
 * No preset-specific suffix — matches the format the maintainer specified exactly.
 */
export function buildFilename(fields: TagFields): string {
  const parts: string[] = []
  if (fields.date.trim()) parts.push(fields.date.trim())
  if (fields.text.trim()) parts.push(toFilenameText(fields.text.trim()))

  const title = fields.title.trim()
  const speaker = fields.speaker.trim()
  const titlePart = title && speaker ? `${title} (${speaker})` : title || (speaker ? `(${speaker})` : '')
  if (titlePart) parts.push(titlePart)

  const joined = parts.length > 0 ? parts.join(' - ') : 'sermon'
  return sanitizeForFilename(joined)
}

/**
 * Best-effort auto-populate by parsing an uploaded filename that follows
 * "Date - Text - Title (Speaker)" — the field separator is " - " (spaces
 * around the dash) specifically, *not* a bare "-", since scripture ranges
 * like "11v8-12v14" have their own un-spaced dash that would otherwise be
 * ambiguous with the field separator. Degrades gracefully: whatever doesn't
 * match just comes back empty for the user to fill in by hand.
 */
// A title pulled from a filename may come from something like a raw
// YouTube export ("The_Whole_Duty_of_Man_...") — underscores are really
// just spaces there, so clean those up wherever a title gets set.
function cleanTitleText(text: string): string {
  return text.replace(/_/g, ' ').replace(/\s+/g, ' ').trim()
}

export function parseFilenameForTags(fileName: string): Partial<TagFields> {
  const base = fileName.replace(/\.[^./\\]+$/, '').trim()
  let parts = base
    .split(' - ')
    .map((p) => p.trim())
    .filter(Boolean)
  if (parts.length === 0) return {}

  const result: Partial<TagFields> = {}

  if (/^\d{4}\.\d{2}\.\d{2}$/.test(parts[0])) {
    result.date = parts[0]
    parts = parts.slice(1)
  }
  if (parts.length === 0) return result

  const last = parts[parts.length - 1]
  const speakerMatch = last.match(/^(.*)\s\(([^)]+)\)$/)
  if (speakerMatch) {
    result.title = cleanTitleText(speakerMatch[1])
    result.speaker = speakerMatch[2].trim()
  } else {
    result.title = cleanTitleText(last)
  }
  parts = parts.slice(0, -1)

  if (parts.length > 0) {
    result.text = fromFilenameText(parts.join(' - '))
  }

  // Nothing about the "Date - Text - Title (Speaker)" convention matched at
  // all (e.g. a raw YouTube export filename) — fall back to a cleaned-up
  // guess at the title rather than leaving every field blank.
  if (Object.keys(result).length === 0) {
    result.title = cleanTitleText(base)
  }

  return result
}

// ---------- ID3v2.3 tag writer ----------
// Minimal by design: only the three frames this app ever sets. Text frames
// use UTF-16LE with a BOM (encoding byte 0x01) rather than ISO-8859-1
// (0x00), so names/titles with accented or non-Latin characters still come
// through correctly — the extra couple of bytes per frame don't matter here.

function synchsafe(n: number): Uint8Array {
  return new Uint8Array([(n >>> 21) & 0x7f, (n >>> 14) & 0x7f, (n >>> 7) & 0x7f, n & 0x7f])
}

function encodeUtf16leWithBom(text: string): Uint8Array {
  const bytes = new Uint8Array(2 + text.length * 2)
  bytes[0] = 0xff
  bytes[1] = 0xfe
  for (let i = 0; i < text.length; i++) {
    const code = text.charCodeAt(i)
    bytes[2 + i * 2] = code & 0xff
    bytes[2 + i * 2 + 1] = (code >> 8) & 0xff
  }
  return bytes
}

function buildTextFrame(frameId: string, text: string): Uint8Array {
  const encoded = encodeUtf16leWithBom(text)
  const content = new Uint8Array(1 + encoded.length)
  content[0] = 0x01 // UTF-16 with BOM
  content.set(encoded, 1)

  const frame = new Uint8Array(10 + content.length)
  for (let i = 0; i < 4; i++) frame[i] = frameId.charCodeAt(i)
  const size = content.length
  frame[4] = (size >>> 24) & 0xff
  frame[5] = (size >>> 16) & 0xff
  frame[6] = (size >>> 8) & 0xff
  frame[7] = size & 0xff
  // frame[8], frame[9] = flags, left 0
  frame.set(content, 10)
  return frame
}

/** Builds a standalone ID3v2.3 tag, ready to prepend to raw MP3 frame data. */
export function buildId3v2Tag(title: string): Uint8Array {
  const year = String(new Date().getFullYear())
  const frames = [buildTextFrame('TIT2', title), buildTextFrame('TPE1', ARTIST), buildTextFrame('TYER', year)]
  const framesSize = frames.reduce((sum, f) => sum + f.length, 0)

  const header = new Uint8Array(10)
  header[0] = 0x49 // 'I'
  header[1] = 0x44 // 'D'
  header[2] = 0x33 // '3'
  header[3] = 0x03 // version 2.3
  header[4] = 0x00
  header[5] = 0x00 // flags
  header.set(synchsafe(framesSize), 6)

  const tag = new Uint8Array(header.length + framesSize)
  tag.set(header, 0)
  let offset = header.length
  for (const frame of frames) {
    tag.set(frame, offset)
    offset += frame.length
  }
  return tag
}
