import { amountLabel } from '../engine/settings'

// A 0-100 slider with the named positions underneath and a marker where Auto put it.
export default function AmountSlider({
  id,
  label,
  value,
  onChange,
  auto,
  marks,
  className,
}: {
  id: string
  label: string
  value: number
  onChange: (v: number) => void
  /** Where Auto suggests the slider should sit, drawn as a small marker on the track. */
  auto?: number
  /** Names for the positions 0, 25, 50, 75 and 100, left to right. */
  marks: string[]
  className?: string
}) {
  const named = [0, 25, 50, 75, 100].indexOf(Math.round(value))
  const text = named >= 0 ? marks[named] : amountLabel(value)
  return (
    <div className={`amount-slider ${className ?? ''}`}>
      <div className="amount-track">
        <input
          id={id}
          type="range"
          min={0}
          max={100}
          step={1}
          value={value}
          aria-label={label}
          aria-valuetext={named >= 0 ? text : `${Math.round(value)} out of 100`}
          onChange={(e) => onChange(Number(e.target.value))}
        />
        {auto !== undefined && (
          <span
            className="auto-marker"
            style={{ left: `calc(${auto}% + ${8 - auto * 0.16}px)` }}
            title={`Auto suggests ${amountLabel(auto)}`}
            aria-hidden
          />
        )}
      </div>
      <div className="amount-ticks" aria-hidden>
        {marks.map((m, i) => (
          <button key={m} type="button" tabIndex={-1} className={named === i ? 'on' : ''} onClick={() => onChange(i * 25)}>
            {m}
          </button>
        ))}
      </div>
    </div>
  )
}
