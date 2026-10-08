import { Plus } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { Link } from 'react-router'
import type { RobotGroup, ServiceCardView } from '../../api/types'
import { Card } from '../../components/ui/Card'
import { useCan, useCanAccessService } from '../../domain/access'
import { ServiceIcon, useServiceName } from '../../domain/services'
import { cn } from '../../lib/cn'
import { useUi } from '../../lib/ui'

const GROUPS: RobotGroup[] = ['active', 'idle', 'charging', 'paused', 'maintenance', 'offline', 'error']

/** One service: its robots by state, its tasks of today and the figure of its own kind. */
export function ServiceCard({ card }: { card: ServiceCardView }) {
  const { t } = useTranslation()
  const name = useServiceName()
  const canCreate = useCan('task.create')
  const allowed = useCanAccessService(card.id)
  const open = useUi((s) => s.openCreateTask)
  const shown = GROUPS.filter((g) => card.byStatus[g] > 0)
  const actionable = card.id !== 'other' && allowed && canCreate && card.enabled
  return (
    <Card className={cn('flex flex-col gap-3 p-4 sm:p-5', !card.enabled && 'opacity-60')}>
      <div className="flex items-center gap-3">
        <span className="flex size-10 items-center justify-center rounded-xl bg-brand-50 text-brand-600"><ServiceIcon service={card.id === 'other' ? null : card.id} className="size-5" /></span>
        <div className="min-w-0 flex-1">
          <Link to={card.id === 'other' ? '/fleet' : `/fleet?service=${card.id}`} className="block truncate font-bold text-slate-900 hover:underline">{name(card.id)}</Link>
          <p className="text-sm text-slate-500">{t('overview.robotsCount', { count: card.robots })}</p>
        </div>
        {actionable && (
          <button type="button" onClick={() => open(card.id)} aria-label={t('tasks.createService', { service: name(card.id) })} title={t('tasks.createService', { service: name(card.id) })}
            className="flex size-9 items-center justify-center rounded-xl text-brand-600 ring-1 ring-brand-200 hover:bg-brand-50"><Plus className="size-4" /></button>
        )}
      </div>
      <ul className="flex flex-wrap gap-x-4 gap-y-1 text-sm">
        {shown.length === 0 && <li className="text-slate-500">{t('overview.noRobots')}</li>}
        {shown.map((g) => (
          <li key={g} className="text-slate-600"><span className="font-semibold text-slate-900 tabular-nums">{card.byStatus[g]}</span> {t(`robotGroup.${g}`)}</li>
        ))}
      </ul>
      {card.id !== 'other' && (
        <div className="mt-auto flex flex-wrap gap-x-5 gap-y-1 border-t border-slate-100 pt-3 text-sm">
          <span><b className="text-slate-900 tabular-nums">{card.tasksToday}</b> <span className="text-slate-600">{t('overview.tasksToday')}</span></span>
          {card.metrics.filter((m) => m.key !== 'completedToday' || card.tasksToday > 0).map((m) => (
            <span key={m.key}><b className="text-slate-900 tabular-nums">{m.value}</b> <span className="text-slate-600">{t(`overview.metric.${m.key}`)}</span></span>
          ))}
          {!card.available && <span className="text-amber-700">{t('tasks.noFleetShort')}</span>}
          {!card.enabled && <span className="text-slate-500">{t('overview.disabled')}</span>}
        </div>
      )}
    </Card>
  )
}
