import type { ReactNode } from 'react'

export interface RailItem {
  id: string
  n: number
  title: string
  sub: string
  state: 'done' | 'on' | 'todo'
  current?: boolean
  disabled?: boolean
  hint?: string
  tour?: string
  onClick?: () => void
}

/** The numbered step list that lives in the window's left sidebar on every screen. */
export default function Rail({ items, foot }: { items: RailItem[]; foot?: ReactNode }) {
  return (
    <nav className="rail" aria-label="Steps">
      <ol>
        {items.map((it) => {
          const inner = (
            <>
              <span className={`rail-dot ${it.state}`} aria-hidden>{it.state === 'done' ? '✓' : it.n}</span>
              <span className="rail-text"><b>{it.title}</b><small title={it.sub}>{it.sub}</small></span>
            </>
          )
          return (
            <li key={it.id}>
              {it.onClick ? (
                <button type="button" className={`rail-item ${it.current ? 'current' : ''}`} aria-current={it.current ? 'step' : undefined} disabled={it.disabled} title={it.hint} data-tour={it.tour} onClick={it.onClick}>{inner}</button>
              ) : (
                <div className={`rail-item static ${it.current ? 'current' : ''}`} aria-current={it.current ? 'step' : undefined}>{inner}</div>
              )}
            </li>
          )
        })}
      </ol>
      {foot && <div className="rail-foot">{foot}</div>}
    </nav>
  )
}
