import {
  BatteryCharging, CalendarClock, CircleCheck, CircleDashed, CircleDot, CirclePause, CircleX, Hourglass, Navigation, OctagonAlert,
  PlugZap, TriangleAlert, UserCheck, WifiOff, Wrench, type LucideIcon,
} from 'lucide-react'
import { useTranslation } from 'react-i18next'
import type { RobotHealth, RobotStatus, ScheduleStatus, TaskState } from '../api/types'
import { cn } from '../lib/cn'

type Tone = 'neutral' | 'info' | 'active' | 'success' | 'warning' | 'critical'

export const toneClass: Record<Tone, string> = {
  neutral: 'bg-slate-100 text-slate-700 ring-slate-200',
  info: 'bg-brand-50 text-brand-700 ring-brand-200',
  active: 'bg-teal-50 text-teal-700 ring-teal-200',
  success: 'bg-emerald-50 text-emerald-700 ring-emerald-200',
  warning: 'bg-amber-50 text-amber-700 ring-amber-200',
  critical: 'bg-red-50 text-red-700 ring-red-200',
}

/** Marker colors on the map (SVG fills); one per tone. */
export const toneColor: Record<Tone, string> = {
  neutral: '#64748b',
  info: '#1a5cf5',
  active: '#0d9488',
  success: '#059669',
  warning: '#d97706',
  critical: '#dc2626',
}

export const robotStatusMeta: Record<RobotStatus, { icon: LucideIcon; tone: Tone }> = {
  IDLE: { icon: CircleDashed, tone: 'neutral' },
  NAVIGATING: { icon: Navigation, tone: 'info' },
  EXECUTING: { icon: CircleDot, tone: 'active' },
  CHARGING: { icon: BatteryCharging, tone: 'success' },
  PAUSED: { icon: CirclePause, tone: 'warning' },
  MAINTENANCE: { icon: Wrench, tone: 'warning' },
  OFFLINE: { icon: WifiOff, tone: 'neutral' },
  ERROR: { icon: OctagonAlert, tone: 'critical' },
}

/** Ring color of each robot status on the map and in every legend; the status name always shows beside it. */
export const robotStatusColor: Record<RobotStatus, string> = {
  IDLE: '#94a3b8',
  NAVIGATING: '#3b82f6',
  EXECUTING: '#22b8cf',
  CHARGING: '#22c55e',
  PAUSED: '#f59e0b',
  MAINTENANCE: '#d97706',
  OFFLINE: '#64748b',
  ERROR: '#ef4444',
}

export const taskStateMeta: Record<TaskState, { icon: LucideIcon; tone: Tone }> = {
  SCHEDULED: { icon: CalendarClock, tone: 'neutral' },
  QUEUED: { icon: Hourglass, tone: 'warning' },
  ASSIGNED: { icon: UserCheck, tone: 'info' },
  EXECUTING: { icon: CircleDot, tone: 'active' },
  PAUSED: { icon: CirclePause, tone: 'warning' },
  COMPLETED: { icon: CircleCheck, tone: 'success' },
  CANCELLED: { icon: CircleX, tone: 'neutral' },
  FAILED: { icon: TriangleAlert, tone: 'critical' },
}

export const scheduleStatusMeta: Record<ScheduleStatus, Tone> = {
  completed: 'success',
  in_progress: 'active',
  scheduled: 'info',
  pending: 'warning',
  failed: 'critical',
  cancelled: 'neutral',
}

/** Tone of a schedule item: its status, except that an open maintenance window uses the warning hue. */
export function scheduleItemTone(item: { type: string; status: ScheduleStatus }): Tone {
  return item.type === 'maintenance' && item.status !== 'completed' && item.status !== 'cancelled' ? 'warning' : scheduleStatusMeta[item.status]
}

export const healthTone: Record<RobotHealth, Tone> = { healthy: 'success', warning: 'warning', critical: 'critical' }

function Chip({ icon: Icon, tone, label, className }: { icon?: LucideIcon; tone: Tone; label: string; className?: string }) {
  return (
    <span className={cn('inline-flex h-6 shrink-0 items-center gap-1 rounded-full px-2 text-xs font-semibold whitespace-nowrap ring-1 ring-inset', toneClass[tone], className)}>
      {Icon && <Icon className="size-3.5" aria-hidden />}
      {label}
    </span>
  )
}

export function RobotStatusBadge({ status, className }: { status: RobotStatus; className?: string }) {
  const { t } = useTranslation()
  const meta = robotStatusMeta[status]
  return <Chip icon={meta.icon} tone={meta.tone} label={t(`robotStatus.${status}`)} className={className} />
}

export function TaskStatusBadge({ state, className }: { state: TaskState; className?: string }) {
  const { t } = useTranslation()
  const meta = taskStateMeta[state]
  return <Chip icon={meta.icon} tone={meta.tone} label={t(`taskState.${state}`)} className={className} />
}

export function ScheduleStatusBadge({ status, tone, className }: { status: ScheduleStatus; tone?: Tone; className?: string }) {
  const { t } = useTranslation()
  return <Chip tone={tone ?? scheduleStatusMeta[status]} label={t(`scheduleStatus.${status}`)} className={className} />
}

export function HealthBadge({ health, className }: { health: RobotHealth; className?: string }) {
  const { t } = useTranslation()
  return <Chip icon={health === 'healthy' ? CircleCheck : health === 'warning' ? TriangleAlert : OctagonAlert} tone={healthTone[health]}
    label={t(`health.${health}`)} className={className} />
}

/** Battery level as a bar and a number; orange below 20 %, red only below 10 % (see the comment inside). */
export function BatteryIndicator({ value, charging, className }: { value: number | null; charging?: boolean; className?: string }) {
  if (value === null) return <span className="text-slate-400">—</span>
  const pct = Math.max(0, Math.min(100, Math.round(value)))
  // Same steps as the backend's battery alerts (alerts.battery_low_pct 20 %, battery_critical_pct 10 %): orange when low,
  // red only when critical; the number is always written beside the bar.
  const level = pct < 10 ? 'critical' : pct < 20 ? 'low' : pct < 40 ? 'fair' : 'good'
  const color = { critical: 'bg-red-500', low: 'bg-amber-500', fair: 'bg-amber-400', good: 'bg-emerald-500' }[level]
  return (
    <span className={cn('inline-flex items-center gap-2 tabular-nums', className)}>
      <span className="relative h-2 w-10 overflow-hidden rounded-full bg-slate-200" aria-hidden>
        <span className={cn('absolute inset-y-0 left-0 rounded-full', color)} style={{ width: `${pct}%` }} />
      </span>
      <span className={cn('text-sm font-semibold', level === 'critical' ? 'text-red-700' : level === 'low' ? 'text-amber-700' : 'text-slate-800')}>{pct}%</span>
      {charging && <PlugZap className="size-3.5 text-emerald-600" aria-hidden />}
    </span>
  )
}
