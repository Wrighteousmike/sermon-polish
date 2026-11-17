// Update check: asks GitHub for the newest public release and compares it with the running version.
// Nothing is sent but the request itself; nothing is installed automatically (the app is unsigned),
// the person is pointed at the download page instead.
const LATEST = 'https://api.github.com/repos/Wrighteousmike/sermon-polish/releases/latest'

export interface UpdateInfo {
  version: string // e.g. "0.3.1"
  url: string // the release page
}

/** "v0.3.1" -> [0, 3, 1]. Anything after a dash (pre-release tags) is ignored. */
export function parseVersion(v: string): number[] {
  return v.trim().replace(/^v/i, '').split('-')[0].split('.').map((n) => parseInt(n, 10) || 0)
}

/** > 0 when a is newer than b. */
export function compareVersions(a: string, b: string): number {
  const x = parseVersion(a)
  const y = parseVersion(b)
  for (let i = 0; i < Math.max(x.length, y.length); i++) {
    const d = (x[i] ?? 0) - (y[i] ?? 0)
    if (d !== 0) return d
  }
  return 0
}

/** The newest release if it is newer than `current`, null if up to date. Throws if it could not be checked. */
export async function checkForUpdate(current: string): Promise<UpdateInfo | null> {
  const fake = import.meta.env.DEV ? new URLSearchParams(window.location.search).get('fakeupdate') : null
  if (fake) return compareVersions(fake, current) > 0 ? { version: fake, url: 'https://github.com/Wrighteousmike/sermon-polish/releases' } : null
  const ctl = new AbortController()
  const timer = setTimeout(() => ctl.abort(), 8000)
  try {
    const res = await fetch(LATEST, { headers: { Accept: 'application/vnd.github+json' }, signal: ctl.signal })
    if (!res.ok) throw new Error(`GitHub answered ${res.status}`)
    const data = (await res.json()) as { tag_name?: string; html_url?: string; draft?: boolean; prerelease?: boolean }
    if (!data.tag_name || data.draft || data.prerelease) return null
    return compareVersions(data.tag_name, current) > 0
      ? { version: data.tag_name.replace(/^v/i, ''), url: data.html_url ?? 'https://github.com/Wrighteousmike/sermon-polish/releases/latest' }
      : null
  } finally {
    clearTimeout(timer)
  }
}
