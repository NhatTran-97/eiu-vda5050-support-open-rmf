// Building blocks of the light operations workspace (EIU design brief): 12px cards with a 1px border, large KPI
// values with compact context, status bars that always carry their numbers in text.
import type { LucideIcon } from 'lucide-react'
import type { ReactNode } from 'react'
import { Link } from 'react-router'
import type { RobotGroup, RobotStatus } from '../../api/types'
import { Skeleton } from '../../components/ui/States'
import { robotStatusColor } from '../../domain/status'
import { cn } from '../../lib/cn'

/** Robot status colors: the shared ring colors of the map. */
export const opsStatusColor: Record<RobotStatus, string> = robotStatusColor


export const opsGroupColor: Record<RobotGroup, string> = {
  active: robotStatusColor.NAVIGATING,
  idle: robotStatusColor.IDLE,
  charging: robotStatusColor.CHARGING,
  paused: robotStatusColor.PAUSED,
  maintenance: robotStatusColor.MAINTENANCE,
  offline: robotStatusColor.OFFLINE,
  error: robotStatusColor.ERROR,
}

export type OpsTone = 'blue' | 'cyan' | 'green' | 'amber' | 'red'

const chip: Record<OpsTone, string> = {
  blue: 'bg-ops-blue-soft text-ops-blue',
  cyan: 'bg-ops-cyan-soft text-ops-cyan',
  green: 'bg-ops-green-soft text-ops-green',
  amber: 'bg-ops-amber-soft text-ops-amber',
  red: 'bg-ops-red-soft text-ops-red',
}

export function OpsCard({ className, children, as: Tag = 'section', ...rest }: { className?: string; children: ReactNode; as?: 'section' | 'div' } & React.HTMLAttributes<HTMLElement>) {
  return <Tag className={cn('rounded-xl border border-ops-border bg-ops-card shadow-ops', className)} {...rest}>{children}</Tag>
}

export function OpsCardHeader({ icon: Icon, title, meta, action, className }: {
  icon?: LucideIcon
  title: ReactNode
  meta?: ReactNode
  action?: ReactNode
  className?: string
}) {
  return (
    <div className={cn('flex flex-wrap items-center gap-x-3 gap-y-2', className)}>
      {Icon && <Icon className="size-5 shrink-0 text-ops-muted" strokeWidth={1.75} aria-hidden />}
      <h2 className="text-section font-semibold text-ops-text">{title}</h2>
      {meta && <div className="flex items-center gap-2 text-meta text-ops-muted">{meta}</div>}
      {action && <div className="ml-auto flex items-center gap-2">{action}</div>}
    </div>
  )
}

/** Small cyan pulse: the data on this card is live. */
export function LiveDot({ className }: { className?: string }) {
  return <span className={cn('live-dot relative inline-block size-2 rounded-full bg-ops-cyan text-ops-cyan', className)} aria-hidden />
}

export function OpsLink({ to, children }: { to: string; children: ReactNode }) {
  return <Link to={to} className="text-control font-medium text-ops-blue hover:underline">{children}</Link>
}

/** KPI icon colors: icon stroke and a 10 % tint of it behind. */
const KPI_ICON: Record<OpsTone, { color: string; background: string }> = {
  blue: { color: '#2563eb', background: 'rgba(37, 99, 235, 0.10)' },
  cyan: { color: '#20a4c4', background: 'rgba(32, 164, 196, 0.10)' },
  green: { color: '#5b8f45', background: 'rgba(91, 143, 69, 0.10)' },
  amber: { color: '#f59e0b', background: 'rgba(245, 158, 11, 0.10)' },
  red: { color: '#d94b4b', background: 'rgba(217, 75, 75, 0.10)' },
}

/** Text color of a value that needs attention. */
const signal: Record<'amber' | 'red', string> = { amber: 'text-ops-amber', red: 'text-ops-red' }

/** KPI tile: label, a large value, one line of context and an optional bar under it. The icon is a 19 px Lucide
 * outline (stroke 1.85) in a 36 px tinted tile in the top-right corner, level with the label, the same for every KPI. Every card uses the same
 * rhythm from the top (label, value, context, bar or second line), so the parts line up across the row. `alert` marks a value that needs action: it is
 * set larger, bold and in the warning or error color; without it the value stays neutral. `compact` (page KPI tiles)
 * tightens the vertical spacing and sets the value bold. */
export function Kpi({ icon: Icon, tone, label, value, unit, line, extra, to, loading, alert, compact }: {
  icon: LucideIcon
  tone: OpsTone
  label: string
  value: ReactNode
  unit?: ReactNode
  line?: ReactNode
  extra?: ReactNode
  to?: string
  loading?: boolean
  alert?: 'amber' | 'red'
  compact?: boolean
}) {
  const body = (
    <OpsCard as="div" className={cn('relative flex h-full flex-col px-5', compact ? 'py-3' : 'py-4', to && 'transition-colors hover:border-ops-blue/40')}>
      <span className="absolute top-4 right-5 flex size-9 items-center justify-center rounded-[10px]" style={{ background: KPI_ICON[tone].background, color: KPI_ICON[tone].color }}>
        <Icon size={19} strokeWidth={1.85} absoluteStrokeWidth aria-hidden />
      </span>
      <p className="pr-12 text-card-title font-medium text-ops-muted">{label}</p>
      {/* The value row is as tall as an alert value in every card, so the lines under it align across the row. */}
      {loading ? <Skeleton className={cn('h-(--text-kpi-critical) w-24', compact ? 'mt-1.5' : 'mt-2')} /> : (
        <p className={cn('flex h-(--text-kpi-critical) items-end', compact ? 'mt-1.5' : 'mt-2')}>
          <span className={cn('flex items-baseline gap-1.5 tracking-tight tabular-nums',
            alert ? cn('text-kpi-critical font-bold', signal[alert]) : cn('text-kpi text-ops-text', compact ? 'font-bold' : 'font-semibold'))}>
            {value}{unit && <span className="text-kpi-unit font-medium text-ops-muted">{unit}</span>}
          </span>
        </p>
      )}
      {line && <p className={cn('text-body text-ops-muted', compact ? 'mt-2.5' : 'mt-3.5')}>{line}</p>}
      {extra && <div className="mt-1">{extra}</div>}
    </OpsCard>
  )
  return to ? <Link to={to} className="block h-full rounded-xl focus-visible:outline-2">{body}</Link> : body
}

/** A count inside a supporting line; it turns semibold in the warning or error color while it is above zero. */
export function SignalCount({ value, tone, children }: { value: number; tone: 'amber' | 'red'; children: ReactNode }) {
  return <span className={cn(value > 0 && cn('font-semibold', signal[tone]))}>{children}</span>
}

/** Part-to-whole bar of robot states; the counts are written beside it, so color is never the only cue. */
export function SegmentBar({ parts, className }: { parts: { key: string; value: number; color: string; label: string }[]; className?: string }) {
  const total = parts.reduce((n, p) => n + p.value, 0)
  if (total === 0) return <div className={cn('h-1.5 rounded-full bg-ops-subtle', className)} />
  return (
    <div className={cn('flex h-1.5 gap-0.5 overflow-hidden rounded-full', className)} role="img"
      aria-label={parts.filter((p) => p.value).map((p) => `${p.value} ${p.label}`).join(', ')}>
      {parts.filter((p) => p.value > 0).map((p) => (
        <span key={p.key} title={`${p.label}: ${p.value}`} style={{ flexGrow: p.value, background: p.color }} />
      ))}
    </div>
  )
}

export function OpsChip({ tone, children, className }: { tone: OpsTone | 'neutral'; children: ReactNode; className?: string }) {
  return (
    <span className={cn('inline-flex h-6 shrink-0 items-center gap-1 rounded-md px-2 text-meta font-medium',
      tone === 'neutral' ? 'bg-ops-subtle text-ops-muted' : chip[tone], className)}>{children}</span>
  )
}
