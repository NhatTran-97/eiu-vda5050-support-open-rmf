import { CheckCheck, ChevronRight } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import type { OpsAlert } from '../../api/types'
import { SeverityChip, severityStyle, useAlertText } from '../operations/alertMeta'
import { cn } from '../../lib/cn'
import { useNow } from '../../lib/useNow'
import { useUi } from '../../lib/ui'

function since(ms: number, t: (k: string, o?: Record<string, unknown>) => string): string {
  const minutes = Math.max(0, Math.round(ms / 60_000))
  if (minutes < 1) return t('time.lessThanMinute')
  if (minutes < 60) return t('time.minutes', { count: minutes })
  return t('alerts.hours', { count: Math.round(minutes / 6) / 10 })
}

/** One alert as a card: severity, what and where; a click highlights its robot and opens its task. */
export function AlertCard({ alert }: { alert: OpsAlert }) {
  const { t } = useTranslation()
  const text = useAlertText()
  const now = useNow(30_000)
  const openRobot = useUi((s) => s.openRobot)
  const openTask = useUi((s) => s.openTask)
  const { title, detail } = text(alert)
  const clickable = !!alert.robot || alert.deliveryId !== null
  return (
    <li>
      <button type="button" disabled={!clickable}
        onClick={() => {
          if (alert.robot) openRobot(alert.robot)
          if (alert.deliveryId !== null) openTask(alert.deliveryId)
        }}
        className={cn('flex w-full items-start gap-3 rounded-xl border-l-4 bg-slate-50 px-3 py-2.5 text-left', severityStyle[alert.severity].row,
          clickable && 'hover:bg-slate-100')}>
        <span className="min-w-0 flex-1">
          <span className="flex flex-wrap items-center gap-2">
            {alert.robot && <span className="text-body font-bold text-slate-900">{alert.robot}</span>}
            <SeverityChip severity={alert.severity} />
          </span>
          <span className="mt-0.5 block text-body font-semibold text-slate-900">{title}</span>
          {(detail || alert.deliveryId !== null) && (
            <span className="block truncate text-meta text-slate-600">
              {alert.deliveryId !== null && `${t('overview.task')} #${alert.deliveryId} · `}{detail}
            </span>
          )}
          <span className="block text-meta text-slate-500">{t('alerts.openedAgo', { time: since(now - alert.openedAt, t) })}</span>
        </span>
        {clickable && <ChevronRight className="mt-1 size-4 shrink-0 text-slate-400" />}
      </button>
    </li>
  )
}

export function NeedsAttention({ alerts }: { alerts: OpsAlert[] }) {
  const { t } = useTranslation()
  if (alerts.length === 0) {
    return <p className="flex items-center gap-2 rounded-xl bg-emerald-50 px-3 py-3 text-sm font-medium text-emerald-700"><CheckCheck className="size-4" />{t('empty.noAlerts')}</p>
  }
  return <ul className="flex flex-col gap-2">{alerts.map((a) => <AlertCard key={a.id} alert={a} />)}</ul>
}
