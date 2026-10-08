import { BellRing, Bot, Check, CheckCheck, Server, X } from 'lucide-react'
import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import { useAlertAction, useAlerts } from '../../api/queries'
import type { OpsAlert } from '../../api/types'
import { Button } from '../../components/ui/Button'
import { Card, CardHeader } from '../../components/ui/Card'
import { EmptyState, ErrorState, Skeleton } from '../../components/ui/States'
import { Tabs } from '../../components/ui/Tabs'
import { cn } from '../../lib/cn'
import { useErrorText } from '../../lib/errors'
import { useFormat } from '../../lib/format'
import { toast } from '../../lib/toast'
import { useNow } from '../../lib/useNow'
import { usePermission } from '../auth/guards'
import { useUi } from '../../lib/ui'
import { SeverityChip, severityStyle, useAlertText } from './alertMeta'

function since(ms: number, t: (k: string, o?: Record<string, unknown>) => string): string {
  const minutes = Math.max(0, Math.round(ms / 60_000))
  if (minutes < 1) return t('time.lessThanMinute')
  if (minutes < 60) return t('time.minutes', { count: minutes })
  return t('alerts.hours', { count: Math.round(minutes / 6) / 10 })
}

/** One alert with its state and the operator actions it allows. */
export function AlertRow({ alert, compact, now }: { alert: OpsAlert; compact?: boolean; now: number }) {
  const { t } = useTranslation()
  const format = useFormat()
  const text = useAlertText()
  const canControl = usePermission('alerts.ack')
  const action = useAlertAction()
  const errorText = useErrorText()
  const openRobot = useUi((s) => s.openRobot)
  const openTask = useUi((s) => s.openTask)
  const { title, detail } = text(alert)
  const open = alert.resolvedAt === null
  const closable = open && (alert.kind === 'event' || !alert.active)
  const act = (kind: 'ack' | 'resolve') => action.mutate({ id: alert.id, action: kind }, { onError: (e) => toast.error(errorText(e)) })
  const target = alert.deliveryId !== null ? () => openTask(alert.deliveryId) : alert.robot ? () => openRobot(alert.robot) : null

  return (
    <li className={cn('flex flex-wrap items-start gap-3 border-l-4 py-3 pr-1 pl-4', severityStyle[alert.severity].row, !open && 'opacity-70')}>
      <div className="min-w-0 flex-1">
        {/* Severity, then where the alert comes from: a robot, or the platform itself. */}
        <div className="flex flex-wrap items-center gap-2">
          <SeverityChip severity={alert.severity} />
          <span className="flex items-center gap-1 text-meta text-slate-500">
            {alert.robot ? <Bot className="size-3.5" aria-hidden /> : <Server className="size-3.5" aria-hidden />}
            {alert.robot ?? t('overview.system')}
          </span>
        </div>
        {target ? (
          <button type="button" onClick={target} className="mt-1 block text-left text-card-title font-semibold text-slate-900 hover:underline">{title}</button>
        ) : (
          <p className="mt-1 text-card-title font-semibold text-slate-900">{title}</p>
        )}
        {detail && !compact && <p className="mt-0.5 text-body text-slate-600">{detail}</p>}
        <p className="mt-1 text-meta text-slate-500">
          {open
            ? t(alert.active && alert.kind === 'condition' ? 'alerts.activeFor' : 'alerts.openedAgo', { time: since(now - alert.openedAt, t) })
            : alert.resolvedBy ? t('alerts.closedBy', { name: alert.resolvedBy, at: format.dateTime(alert.resolvedAt!) })
            : t('alerts.ended', { at: format.dateTime(alert.resolvedAt!) })}
          {alert.ackedAt && ` · ${t('alerts.ackedBy', { name: alert.ackedBy ?? '', at: format.time(alert.ackedAt) })}`}
        </p>
      </div>
      {canControl && open && (
        <div className="flex gap-2">
          {!alert.ackedAt && (
            <Button size="sm" variant="secondary" icon={<Check className="size-4" />} loading={action.isPending && action.variables?.action === 'ack'} onClick={() => act('ack')}>
              {t('alerts.ack')}
            </Button>
          )}
          {closable && !compact && (
            <Button size="sm" variant="ghost" icon={<X className="size-4" />} loading={action.isPending && action.variables?.action === 'resolve'} onClick={() => act('resolve')}>
              {t('alerts.resolve')}
            </Button>
          )}
        </div>
      )}
    </li>
  )
}

/** Operations alerts: open ones by severity, unacknowledged first, and the history. */
export function AlertsPanel() {
  const { t } = useTranslation()
  const [state, setState] = useState<'open' | 'all'>('open')
  const alerts = useAlerts(state)
  const counts = useAlerts('open').data?.counts
  const canControl = usePermission('alerts.ack')
  const action = useAlertAction()
  const now = useNow(30_000)

  return (
    <Card className="p-4 sm:p-5">
      <div className="mb-3 flex flex-wrap items-center gap-3">
        <CardHeader icon={<BellRing />} title={t('alerts.title')} subtitle={t('alerts.subtitle')} className="min-w-56 flex-1" />
        {canControl && (counts?.unacked ?? 0) > 0 && (
          <Button size="sm" variant="secondary" icon={<CheckCheck className="size-4" />} loading={action.isPending} onClick={() => action.mutate({ id: 'all', action: 'ack' })}>
            {t('alerts.ackAll', { count: counts?.unacked })}
          </Button>
        )}
      </div>
      <Tabs<'open' | 'all'>
        value={state}
        onChange={setState}
        label={t('alerts.title')}
        items={[{ value: 'open', label: t('alerts.open'), count: counts?.open }, { value: 'all', label: t('alerts.history') }]}
        className="mb-2"
      />
      {alerts.isPending && <Skeleton className="h-40" />}
      {alerts.isError && <ErrorState onRetry={() => void alerts.refetch()} />}
      {alerts.data && alerts.data.items.length === 0 && (
        <EmptyState icon={<CheckCheck />} title={t(state === 'open' ? 'alerts.none' : 'alerts.noHistory')} />
      )}
      {alerts.data && alerts.data.items.length > 0 && (
        <ul className="divide-y divide-slate-100">
          {alerts.data.items.map((a) => <AlertRow key={a.id} alert={a} now={now} />)}
        </ul>
      )}
    </Card>
  )
}
