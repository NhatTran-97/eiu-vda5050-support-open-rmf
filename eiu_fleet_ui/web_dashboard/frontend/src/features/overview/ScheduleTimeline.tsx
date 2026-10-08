import { BatteryCharging, CalendarX2, Wrench } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import type { ScheduleItem } from '../../api/types'
import { ServiceIcon, useServiceMap, useServiceName } from '../../domain/services'
import { scheduleItemTone, ScheduleStatusBadge } from '../../domain/status'
import { cn } from '../../lib/cn'
import { useFormat } from '../../lib/format'
import { useUi } from '../../lib/ui'
import { fromSchedule, taskCode, useTaskPlace } from '../tasks/taskText'

/** Title of a schedule item: service and place of a task, the title of a maintenance item, a charging session. */
export function useScheduleTitle() {
  const { t } = useTranslation()
  const services = useServiceMap()
  const serviceName = useServiceName()
  const { text } = useTaskPlace()
  return (item: ScheduleItem) => {
    if (item.type === 'charging') return t('schedule.charging', { robot: item.robot })
    if (item.type === 'maintenance') return `${item.robot} · ${item.title}`
    const kind = services.get(item.service ?? '')?.category ?? 'delivery'
    return `${serviceName(item.service)} · ${text(fromSchedule(item, kind))}`
  }
}

/** Items of a day in time order: time, what, status; a task opens its drawer. */
export function ScheduleTimeline({ items, className }: { items: ScheduleItem[]; className?: string }) {
  const { t } = useTranslation()
  const format = useFormat()
  const title = useScheduleTitle()
  const openTask = useUi((s) => s.openTask)
  const openRobot = useUi((s) => s.openRobot)
  const selectedTask = useUi((s) => s.task)
  const selectedRobot = useUi((s) => s.robot)
  if (items.length === 0) {
    return <p className="flex items-center gap-2 py-4 text-body text-slate-500"><CalendarX2 className="size-4" />{t('empty.noSchedule')}</p>
  }
  return (
    <ol className={cn('flex flex-col', className)}>
      {items.map((item) => {
        // The row whose task (or, for charging and maintenance, robot) is open in a drawer.
        const selected = item.taskId ? item.taskId === selectedTask : !!item.robot && item.robot === selectedRobot
        return (
        <li key={item.id} data-status={item.status}>
          <button type="button" aria-current={selected || undefined}
            onClick={() => (item.taskId ? openTask(item.taskId) : item.robot && openRobot(item.robot))}
            className={cn('grid w-full grid-cols-[3.5rem_auto_1fr] items-start gap-3 rounded-xl px-2 py-2.5 text-left',
              selected ? 'bg-brand-50 ring-1 ring-brand-200 ring-inset' : 'hover:bg-slate-50')}>
            <span className="pt-0.5 text-body font-bold text-slate-900 tabular-nums">{format.time(item.at)}</span>
            <span className="mt-0.5 flex size-7 items-center justify-center rounded-lg bg-slate-100 text-slate-600">
              {item.type === 'charging' ? <BatteryCharging className="size-4" /> : item.type === 'maintenance' ? <Wrench className="size-4" /> : <ServiceIcon service={item.service} />}
            </span>
            <span className="min-w-0">
              <span className="block truncate text-body font-medium text-slate-900">{title(item)}</span>
              <span className="mt-1 flex flex-wrap items-center gap-2 text-meta text-slate-500">
                <ScheduleStatusBadge status={item.status} tone={scheduleItemTone(item)} />
                {item.taskId && taskCode(item.taskId, item.service ?? 'task')}
                {item.type === 'task' && item.robot && ` · ${item.robot}`}
                {item.repeat && item.repeat !== 'none' && ` · ${t(`repeat.${item.repeat}`)}`}
              </span>
            </span>
          </button>
        </li>
        )
      })}
    </ol>
  )
}
