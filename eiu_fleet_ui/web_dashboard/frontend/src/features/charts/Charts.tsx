import { Table2 } from 'lucide-react'
import { useEffect, useRef, useState, type ReactNode } from 'react'
import { useTranslation } from 'react-i18next'
import { cn } from '../../lib/cn'

export interface Series {
  key: string
  label: string
  /** CSS color of the marks, e.g. var(--color-chart-blue). */
  color: string
}

const HEIGHT = 180
const PAD = { top: 12, right: 12, bottom: 24, left: 36 }
const GAP = 2

function useWidth<T extends HTMLElement>() {
  const ref = useRef<T>(null)
  const [width, setWidth] = useState(0)
  useEffect(() => {
    const el = ref.current
    if (!el) return
    const observer = new ResizeObserver(([entry]) => setWidth(entry.contentRect.width))
    observer.observe(el)
    return () => observer.disconnect()
  }, [])
  return [ref, width] as const
}

/** Clean ticks from 0 to a rounded maximum. */
function ticks(max: number, count = 4): number[] {
  if (max <= 0) return [0, 1]
  const raw = max / count
  const mag = 10 ** Math.floor(Math.log10(raw))
  const step = [1, 2, 2.5, 5, 10].map((m) => m * mag).find((s) => s >= raw) ?? raw
  const out = []
  for (let v = 0; v <= max + step * 0.001; v += step) out.push(Math.round(v * 100) / 100)
  if (out[out.length - 1] < max) out.push(out[out.length - 1] + step)
  return out
}

export function Legend({ series }: { series: Series[] }) {
  return (
    <ul className="flex flex-wrap gap-x-4 gap-y-1 text-xs text-slate-600">
      {series.map((s) => (
        <li key={s.key} className="flex items-center gap-1.5">
          <span className="size-2.5 rounded-sm" style={{ background: s.color }} aria-hidden />{s.label}
        </li>
      ))}
    </ul>
  )
}

function Tooltip({ x, children }: { x: number; children: ReactNode }) {
  return (
    <div className="pointer-events-none absolute top-1 z-10 -translate-x-1/2 rounded-lg bg-surface px-2.5 py-1.5 text-xs whitespace-nowrap text-slate-800 shadow-lg ring-1 ring-slate-900/10"
      style={{ left: x }}>
      {children}
    </div>
  )
}

/** Chart with its legend and a table view of the same numbers. */
export function ChartFrame({ legend, table, children, className }: {
  legend?: Series[]
  table: { head: string[]; rows: (string | number)[][] }
  children: ReactNode
  className?: string
}) {
  const { t } = useTranslation()
  const [asTable, setAsTable] = useState(false)
  return (
    <div className={cn('flex flex-col gap-2', className)}>
      <div className="flex items-center gap-3">
        <div className="min-w-0 flex-1">{legend && legend.length > 1 && <Legend series={legend} />}</div>
        <button type="button" aria-pressed={asTable} onClick={() => setAsTable((v) => !v)}
          className={cn('flex items-center gap-1 rounded-lg px-2 py-1 text-xs font-medium', asTable ? 'bg-brand-50 text-brand-700' : 'text-slate-500 hover:bg-slate-100')}>
          <Table2 className="size-3.5" />{t('charts.table')}
        </button>
      </div>
      {asTable ? (
        <div className="max-h-64 overflow-auto">
          <table className="w-full text-sm">
            <thead><tr>{table.head.map((h) => <th key={h} className="px-2 py-1.5 text-left text-xs font-semibold text-slate-500">{h}</th>)}</tr></thead>
            <tbody className="divide-y divide-slate-100">
              {table.rows.map((r, i) => <tr key={i}>{r.map((c, j) => <td key={j} className="px-2 py-1.5 text-slate-800">{c}</td>)}</tr>)}
            </tbody>
          </table>
        </div>
      ) : children}
    </div>
  )
}

/** Stacked columns per category (e.g. per day), one baseline, 2px surface gaps between segments. */
export function StackedColumns({ data, series, format = (v) => String(v), empty }: {
  data: { label: string; values: Record<string, number> }[]
  series: Series[]
  format?: (v: number) => string
  empty?: string
}) {
  const [ref, width] = useWidth<HTMLDivElement>()
  const [hover, setHover] = useState<number | null>(null)
  const totals = data.map((d) => series.reduce((sum, s) => sum + (d.values[s.key] ?? 0), 0))
  const axis = ticks(Math.max(...totals, 0))
  const max = axis[axis.length - 1] || 1
  const plotW = Math.max(0, width - PAD.left - PAD.right)
  const plotH = HEIGHT - PAD.top - PAD.bottom
  const band = data.length ? plotW / data.length : 0
  const barW = Math.min(24, band * 0.6)
  const y = (v: number) => PAD.top + plotH - (v / max) * plotH
  const everyNth = Math.ceil(data.length / Math.max(1, Math.floor(plotW / 44)))
  return (
    <div ref={ref} className="relative" onMouseLeave={() => setHover(null)}>
      {width > 0 && (
        <svg width={width} height={HEIGHT} role="img">
          {axis.map((v) => (
            <g key={v}>
              <line x1={PAD.left} x2={width - PAD.right} y1={y(v)} y2={y(v)} stroke="var(--color-slate-200)" strokeWidth={1} />
              <text x={PAD.left - 6} y={y(v) + 4} textAnchor="end" className="fill-slate-500 text-[11px]">{format(v)}</text>
            </g>
          ))}
          {data.map((d, i) => {
            const cx = PAD.left + band * i + band / 2
            let base = 0
            const segments = series.filter((s) => (d.values[s.key] ?? 0) > 0)
            return (
              <g key={d.label}>
                {segments.map((s, k) => {
                  const v = d.values[s.key]
                  const top = y(base + v)
                  const bottom = y(base) - (k > 0 ? GAP : 0)
                  base += v
                  const h = Math.max(0, bottom - top)
                  const last = k === segments.length - 1
                  const r = last ? Math.min(4, h / 2, barW / 2) : 0
                  return (
                    <path key={s.key} fill={s.color} opacity={hover === null || hover === i ? 1 : 0.55}
                      d={`M${cx - barW / 2},${bottom} V${top + r} Q${cx - barW / 2},${top} ${cx - barW / 2 + r},${top} H${cx + barW / 2 - r} Q${cx + barW / 2},${top} ${cx + barW / 2},${top + r} V${bottom} Z`} />
                  )
                })}
                {i % everyNth === 0 && <text x={cx} y={HEIGHT - 6} textAnchor="middle" className="fill-slate-500 text-[11px]">{d.label}</text>}
                <rect x={cx - band / 2} y={PAD.top} width={band} height={plotH} fill="transparent" onMouseEnter={() => setHover(i)} />
              </g>
            )
          })}
        </svg>
      )}
      {hover !== null && data[hover] && (
        <Tooltip x={Math.min(Math.max(PAD.left + band * hover + band / 2, 70), width - 70)}>
          <p className="mb-1 font-semibold">{data[hover].label}</p>
          {series.map((s) => (
            <p key={s.key} className="flex items-center gap-1.5">
              <span className="size-2 rounded-sm" style={{ background: s.color }} />{s.label}: <b>{format(data[hover].values[s.key] ?? 0)}</b>
            </p>
          ))}
        </Tooltip>
      )}
      {empty && totals.every((v) => v === 0) && <p className="absolute inset-0 flex items-center justify-center text-sm text-slate-500">{empty}</p>}
    </div>
  )
}

/** One series over time with a 10% area wash, a crosshair and the last value labelled. */
export function TimeLine({ points, color, max, unit, formatTime, empty }: {
  points: [number, number][]
  color: string
  max: number
  unit: string
  formatTime: (ms: number) => string
  empty: string
}) {
  const [ref, width] = useWidth<HTMLDivElement>()
  const [hover, setHover] = useState<number | null>(null)
  // Room on the right for the last value's label.
  const plotW = Math.max(0, width - PAD.left - PAD.right - 48)
  const plotH = HEIGHT - PAD.top - PAD.bottom
  const t0 = points[0]?.[0] ?? 0
  const t1 = points[points.length - 1]?.[0] ?? 1
  const x = (t: number) => PAD.left + (t1 === t0 ? plotW : ((t - t0) / (t1 - t0)) * plotW)
  const y = (v: number) => PAD.top + plotH - (v / max) * plotH
  const axis = ticks(max)
  const line = points.map(([t, v], i) => `${i ? 'L' : 'M'}${x(t).toFixed(1)},${y(v).toFixed(1)}`).join('')
  const last = points[points.length - 1]
  const onMove = (e: React.MouseEvent) => {
    if (!points.length) return
    const box = (e.currentTarget as HTMLElement).getBoundingClientRect()
    const px = e.clientX - box.left
    let best = 0
    points.forEach(([t], i) => { if (Math.abs(x(t) - px) < Math.abs(x(points[best][0]) - px)) best = i })
    setHover(best)
  }
  return (
    <div ref={ref} className="relative" onMouseMove={onMove} onMouseLeave={() => setHover(null)}>
      {width > 0 && (
        <svg width={width} height={HEIGHT} role="img">
          {axis.map((v) => (
            <g key={v}>
              <line x1={PAD.left} x2={width - PAD.right} y1={y(v)} y2={y(v)} stroke="var(--color-slate-200)" strokeWidth={1} />
              <text x={PAD.left - 6} y={y(v) + 4} textAnchor="end" className="fill-slate-500 text-[11px]">{v}</text>
            </g>
          ))}
          {points.length > 1 && <>
            <path d={`${line}L${x(t1)},${y(0)}L${x(t0)},${y(0)}Z`} fill={color} opacity={0.1} />
            <path d={line} fill="none" stroke={color} strokeWidth={2} strokeLinejoin="round" strokeLinecap="round" />
            <text x={PAD.left} y={HEIGHT - 6} className="fill-slate-500 text-[11px]">{formatTime(t0)}</text>
            <text x={x(t1)} y={HEIGHT - 6} textAnchor="end" className="fill-slate-500 text-[11px]">{formatTime(t1)}</text>
          </>}
          {last && <>
            <circle cx={x(last[0])} cy={y(last[1])} r={4} fill={color} stroke="var(--color-surface)" strokeWidth={2} />
            <text x={x(last[0]) + 8} y={y(last[1]) + 4} className="fill-slate-800 text-xs font-semibold">{last[1]}{unit}</text>
          </>}
          {hover !== null && points[hover] && (
            <g>
              <line x1={x(points[hover][0])} x2={x(points[hover][0])} y1={PAD.top} y2={PAD.top + plotH} stroke="var(--color-slate-400)" strokeWidth={1} />
              <circle cx={x(points[hover][0])} cy={y(points[hover][1])} r={4.5} fill={color} stroke="var(--color-surface)" strokeWidth={2} />
            </g>
          )}
        </svg>
      )}
      {hover !== null && points[hover] && (
        <Tooltip x={Math.min(Math.max(x(points[hover][0]), 60), width - 60)}>
          <b>{points[hover][1]}{unit}</b> · {formatTime(points[hover][0])}
        </Tooltip>
      )}
      {points.length === 0 && <p className="absolute inset-0 flex items-center justify-center text-sm text-slate-500">{empty}</p>}
    </div>
  )
}

/** Horizontal bars of one series, value at the tip. */
export function Bars({ items, color, format = (v) => String(v) }: {
  items: { label: string; value: number }[]
  color: string
  format?: (v: number) => string
}) {
  const max = Math.max(...items.map((i) => i.value), 1)
  return (
    <ul className="flex flex-col gap-2">
      {items.map((i) => (
        <li key={i.label} className="grid grid-cols-[minmax(6rem,10rem)_1fr] items-center gap-3 text-sm" title={`${i.label}: ${format(i.value)}`}>
          <span className="truncate text-slate-700">{i.label}</span>
          <span className="flex items-center gap-2">
            <span className="h-3 rounded-r-[4px]" style={{ width: `${(i.value / max) * 85}%`, minWidth: i.value ? 3 : 0, background: color }} />
            <span className="text-xs font-semibold text-slate-800">{format(i.value)}</span>
          </span>
        </li>
      ))}
    </ul>
  )
}

/** Several series over the same buckets on one axis, 2px lines, end labels, a crosshair with every value. */
export function MultiLine({ labels, series, values, empty, height: HEIGHT = 180, endLabels = true }: {
  /** Bucket labels on the x axis. */
  labels: string[]
  series: Series[]
  /** values[series.key][bucket] */
  values: Record<string, number[]>
  empty?: string
  /** Chart height in pixels. */
  height?: number
  /** Write each series' last value at the end of its line. */
  endLabels?: boolean
}) {
  const [ref, width] = useWidth<HTMLDivElement>()
  const [hover, setHover] = useState<number | null>(null)
  const all = series.flatMap((s) => values[s.key] ?? [])
  const axis = ticks(Math.max(...all, 0))
  const max = axis[axis.length - 1] || 1
  // Room on the right for the end labels.
  const plotW = Math.max(0, width - PAD.left - PAD.right - (endLabels ? 72 : 16))
  const plotH = HEIGHT - PAD.top - PAD.bottom
  const n = labels.length
  const x = (i: number) => PAD.left + (n <= 1 ? plotW / 2 : (i / (n - 1)) * plotW)
  const y = (v: number) => PAD.top + plotH - (v / max) * plotH
  const everyNth = Math.ceil(n / Math.max(1, Math.floor(plotW / 44)))
  // End labels nudged apart so they do not overlap, kept inside the plot.
  const ends = series.map((s) => ({ s, v: values[s.key]?.[n - 1] ?? 0 })).sort((a, b) => b.v - a.v)
  const placed = ends.map((e) => ({ ...e, yy: y(e.v) + 4 }))
  for (let i = 1; i < placed.length; i++) placed[i].yy = Math.max(placed[i].yy, placed[i - 1].yy + 13)
  const bottom = PAD.top + plotH + 4
  for (let i = placed.length - 1; i >= 0; i--) {
    const limit = i === placed.length - 1 ? bottom : placed[i + 1].yy - 13
    placed[i].yy = Math.min(placed[i].yy, limit)
  }
  const onMove = (e: React.MouseEvent) => {
    if (!n) return
    const box = (e.currentTarget as HTMLElement).getBoundingClientRect()
    const i = Math.round(((e.clientX - box.left - PAD.left) / Math.max(1, plotW)) * (n - 1))
    setHover(Math.max(0, Math.min(n - 1, i)))
  }
  return (
    <div ref={ref} className="relative" onMouseMove={onMove} onMouseLeave={() => setHover(null)}>
      {width > 0 && (
        <svg width={width} height={HEIGHT} role="img">
          {axis.map((v) => (
            <g key={v}>
              <line x1={PAD.left} x2={PAD.left + plotW} y1={y(v)} y2={y(v)} stroke="var(--color-slate-200)" strokeWidth={1} />
              <text x={PAD.left - 6} y={y(v) + 4} textAnchor="end" className="fill-slate-500 text-[11px]">{v}</text>
            </g>
          ))}
          {labels.map((l, i) => i % everyNth === 0 && (
            <text key={i} x={x(i)} y={HEIGHT - 6} textAnchor="middle" className="fill-slate-500 text-[11px]">{l}</text>
          ))}
          {series.map((s) => {
            const pts = (values[s.key] ?? []).map((v, i) => `${i ? 'L' : 'M'}${x(i).toFixed(1)},${y(v).toFixed(1)}`).join('')
            return <path key={s.key} d={pts} fill="none" stroke={s.color} strokeWidth={2} strokeLinejoin="round" strokeLinecap="round" />
          })}
          {n > 0 && endLabels && placed.map(({ s, v, yy }) => (
            <g key={s.key}>
              <circle cx={x(n - 1)} cy={y(v)} r={3.5} fill={s.color} stroke="var(--color-surface)" strokeWidth={2} />
              <text x={x(n - 1) + 8} y={yy} className="fill-slate-700 text-[11px] font-medium">{s.label} {v}</text>
            </g>
          ))}
          {hover !== null && (
            <g>
              <line x1={x(hover)} x2={x(hover)} y1={PAD.top} y2={PAD.top + plotH} stroke="var(--color-slate-400)" strokeWidth={1} />
              {series.map((s) => (
                <circle key={s.key} cx={x(hover)} cy={y(values[s.key]?.[hover] ?? 0)} r={4} fill={s.color} stroke="var(--color-surface)" strokeWidth={2} />
              ))}
            </g>
          )}
        </svg>
      )}
      {hover !== null && labels[hover] !== undefined && (
        <Tooltip x={Math.min(Math.max(x(hover), 70), width - 70)}>
          <p className="mb-1 font-semibold">{labels[hover]}</p>
          {series.map((s) => (
            <p key={s.key} className="flex items-center gap-1.5">
              <span className="size-2 rounded-sm" style={{ background: s.color }} />{s.label}: <b>{values[s.key]?.[hover] ?? 0}</b>
            </p>
          ))}
        </Tooltip>
      )}
      {empty && all.every((v) => v === 0) && <p className="absolute inset-0 flex items-center justify-center text-sm text-slate-500">{empty}</p>}
    </div>
  )
}
