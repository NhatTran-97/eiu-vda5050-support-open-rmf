import { ChevronRight, CircleAlert, CircleCheck, Info, OctagonAlert, Plus } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { Link } from 'react-router'
import type { AlertSeverity, OpsAlert, RobotGroup, ServiceCardView } from '../../api/types'
import { useCan, useCanAccessService } from '../../domain/access'
import { ServiceIcon, useServiceName } from '../../domain/services'
import { cn } from '../../lib/cn'
import { useNow } from '../../lib/useNow'
import { useUi } from '../../lib/ui'
import { useAlertText } from '../operations/alertMeta'
import { OpsCard, OpsChip, opsGroupColor, type OpsTone } from './ops'

const GROUPS: RobotGroup[] = ['active', 'idle', 'charging', 'paused', 'maintenance', 'error', 'offline']

const SEVERITY: Record<AlertSeverity, { tone: OpsTone | 'neutral'; icon: typeof Info; bar: string }> = {
  critical: { tone: 'red', icon: OctagonAlert, bar: 'bg-ops-red' },
  warning: { tone: 'amber', icon: CircleAlert, bar: 'bg-ops-amber' },
  info: { tone: 'neutral', icon: Info, bar: 'bg-ops-muted' },
}

function ago(ms: number, t: (k: string, o?: Record<string, unknown>) => string): string {
  const minutes = Math.max(0, Math.round(ms / 60_000))
  if (minutes < 1) return t('time.lessThanMinute')
  if (minutes < 60) return t('time.minutes', { count: minutes })
  return t('alerts.hours', { count: Math.round(minutes / 6) / 10 })
}

/** One alert: robot, what happened, severity; a click selects the robot (highlighted on the map) and opens its drawer. */
export function AttentionItem({ alert }: { alert: OpsAlert }) {
  const { t } = useTranslation()
  const text = useAlertText()
  const now = useNow(30_000)
  const openRobot = useUi((s) => s.openRobot)
  const openTask = useUi((s) => s.openTask)
  const selected = useUi((s) => s.robot)
  const { title, detail } = text(alert)
  const meta = SEVERITY[alert.severity]
  const Icon = meta.icon
  const clickable = !!alert.robot || alert.deliveryId !== null
  return (
    <li>
      <button type="button" disabled={!clickable}
        onClick={() => {
          if (alert.robot) openRobot(alert.robot)
          else if (alert.deliveryId !== null) openTask(alert.deliveryId)
        }}
        className={cn('relative flex w-full items-start gap-3 overflow-hidden rounded-lg border border-ops-border bg-ops-raised py-3 pr-3 pl-4 text-left transition-colors',
          clickable && 'hover:bg-ops-subtle', alert.robot && alert.robot === selected && 'border-ops-blue ring-1 ring-ops-blue')}>
        <span className={cn('absolute inset-y-0 left-0 w-1', meta.bar)} aria-hidden />
        <span className="min-w-0 flex-1">
          <span className="flex items-center gap-2">
            <span className="truncate text-card-title font-semibold text-ops-text">{alert.robot ?? (alert.deliveryId !== null ? `#${alert.deliveryId}` : t('overview.system'))}</span>
            <OpsChip tone={meta.tone} className="ml-auto"><Icon className="size-3.5" aria-hidden />{t(`alerts.severity.${alert.severity}`)}</OpsChip>
          </span>
          <span className="mt-0.5 block text-body text-ops-text">{title}</span>
          {detail && <span className="block truncate text-meta text-ops-muted">{detail}</span>}
          <span className="mt-1 block text-meta text-ops-muted">{t('alerts.openedAgo', { time: ago(now - alert.openedAt, t) })}</span>
        </span>
        {clickable && <ChevronRight className="mt-0.5 size-4 shrink-0 text-ops-muted" aria-hidden />}
      </button>
    </li>
  )
}

export function AttentionList({ alerts }: { alerts: OpsAlert[] }) {
  const { t } = useTranslation()
  if (alerts.length === 0) {
    return (
      <div className="flex items-center gap-2 rounded-lg bg-ops-green-soft px-3 py-3 text-body font-medium text-ops-green">
        <CircleCheck className="size-4" aria-hidden />{t('empty.noAlerts')}
      </div>
    )
  }
  return <ul className="flex flex-col gap-2">{alerts.map((a) => <AttentionItem key={a.id} alert={a} />)}</ul>
}

/** One robot service the user may operate: robots, a one-line state summary and today's tasks. Task details are on
 * the Tasks and Analytics pages. */
export function ServiceTile({ card }: { card: ServiceCardView }) {
  const { t } = useTranslation()
  const name = useServiceName()
  const canCreate = useCan('task.create')
  const allowed = useCanAccessService(card.id)
  const open = useUi((s) => s.openCreateTask)
  const actionable = card.id !== 'other' && allowed && canCreate && card.enabled
  const parts = GROUPS.filter((g) => card.byStatus[g] > 0)
  const rounds = card.metrics.find((m) => m.key === 'roundsToday')
  return (
    <OpsCard className={cn('flex flex-col gap-2.5 px-3.5 py-3', !card.enabled && 'opacity-60')}>
      <div className="flex items-center gap-3">
        <span className="flex size-9 items-center justify-center rounded-lg bg-ops-blue-soft text-ops-blue">
          <ServiceIcon service={card.id === 'other' ? null : card.id} className="size-4.5" />
        </span>
        <div className="min-w-0 flex-1">
          <Link to={card.id === 'other' ? '/fleet' : `/fleet?service=${card.id}`} className="block truncate text-card-title font-semibold text-ops-text hover:underline">{name(card.id)}</Link>
          <p className="text-meta text-ops-muted">{t('overview.robotsCount', { count: card.robots })}</p>
        </div>
        {actionable && (
          <button type="button" onClick={() => open(card.id)} title={t('tasks.createService', { service: name(card.id) })}
            aria-label={t('tasks.createService', { service: name(card.id) })}
            className="flex size-8 items-center justify-center rounded-lg border border-ops-border text-ops-blue hover:bg-ops-blue-soft">
            <Plus className="size-4" />
          </button>
        )}
      </div>
      <p className="flex flex-wrap gap-x-3 gap-y-1 text-body text-ops-muted">
        {card.robots === 0 && <span>{t('overview.noRobots')}</span>}
        {parts.map((g) => (
          <span key={g} className="flex items-center gap-1.5">
            <span className="size-2 rounded-full" style={{ background: opsGroupColor[g] }} aria-hidden />
            <b className="font-semibold text-ops-text tabular-nums">{card.byStatus[g]}</b> {g === 'idle' ? t('overview.tile.ready') : t(`robotGroup.${g}`)}
          </span>
        ))}
      </p>
      {card.id !== 'other' && (
        <p className="mt-auto border-t border-ops-border pt-2.5 text-body text-ops-text">
          {t('overview.tile.tasksToday', { count: card.tasksToday })}
          {rounds && <span className="text-ops-muted"> · {t('overview.tile.roundsToday', { count: rounds.value })}</span>}
          {(!card.available || !card.enabled) && <span className="font-medium text-ops-amber"> · {!card.enabled ? t('overview.disabled') : t('tasks.noFleetShort')}</span>}
        </p>
      )}
    </OpsCard>
  )
}
