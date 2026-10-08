import { BatteryCharging, CalendarDays, ChevronLeft, ChevronRight, Wrench } from 'lucide-react'
import { useMemo, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { useSearchParams } from 'react-router'
import { useCatalog, useFleetRobots, useSchedule } from '../api/queries'
import type { ScheduleItem } from '../api/types'
import { Button, IconButton } from '../components/ui/Button'
import { Card } from '../components/ui/Card'
import { Select } from '../components/ui/Field'
import { FilterTabs } from '../components/ui/FilterTabs'
import { ErrorState, Skeleton } from '../components/ui/States'
import { useAllowedServices, useCan } from '../domain/access'
import { ServiceIcon, useServiceName } from '../domain/services'
import { scheduleItemTone, toneClass } from '../domain/status'
import { ScheduleTimeline, useScheduleTitle } from '../features/overview/ScheduleTimeline'
import { CreateTaskButton, createTaskInHeader } from '../features/tasks/CreateTaskDialog'
import { PageHeader } from '../layout/PageHeader'
import { cn } from '../lib/cn'
import { useFormat } from '../lib/format'
import { useLocalize } from '../lib/i18nText'
import { useUi } from '../lib/ui'

type View = 'day' | 'week' | 'month'
const DAY = 86_400_000

function startOfDay(ms: number): number {
  const d = new Date(ms)
  d.setHours(0, 0, 0, 0)
  return d.getTime()
}

/** [start, end) of the view around `anchor`; weeks start on Monday. */
function range(view: View, anchor: number): [number, number] {
  const d = new Date(startOfDay(anchor))
  if (view === 'day') return [d.getTime(), d.getTime() + DAY]
  if (view === 'week') {
    const back = (d.getDay() + 6) % 7
    const start = startOfDay(d.getTime() - back * DAY + DAY / 2)
    const end = new Date(start)
    end.setDate(end.getDate() + 7)
    return [start, end.getTime()]
  }
  const start = new Date(d.getFullYear(), d.getMonth(), 1).getTime()
  return [start, new Date(d.getFullYear(), d.getMonth() + 1, 1).getTime()]
}

function shift(view: View, anchor: number, step: number): number {
  const d = new Date(anchor)
  if (view === 'day') d.setDate(d.getDate() + step)
  else if (view === 'week') d.setDate(d.getDate() + 7 * step)
  else d.setMonth(d.getMonth() + step)
  return d.getTime()
}

function days(start: number, end: number): number[] {
  const out: number[] = []
  for (let d = new Date(start); d.getTime() < end; d.setDate(d.getDate() + 1)) out.push(d.getTime())
  return out
}

/** One item of the week or month grid: time first, the service icon, then a short label (week: the item's title,
 * month: only the service or kind); the full title is the tooltip and a click opens the drawer. */
function Chip({ item, compact }: { item: ScheduleItem; compact?: boolean }) {
  const { t } = useTranslation()
  const format = useFormat()
  const title = useScheduleTitle()
  const serviceName = useServiceName()
  const openTask = useUi((s) => s.openTask)
  const openRobot = useUi((s) => s.openRobot)
  const selected = useUi((s) => (item.taskId ? s.task === item.taskId : !!item.robot && s.robot === item.robot))
  // The icon already names the service, so the week label drops the "Service · " prefix of the title.
  const full = title(item)
  const prefix = `${serviceName(item.service)} · `
  const weekLabel = item.type === 'task' && full.startsWith(prefix) ? full.slice(prefix.length) : full
  const kind = item.type === 'charging' ? t('robotStatus.CHARGING') : item.type === 'maintenance' ? t('nav.maintenance') : serviceName(item.service)
  return (
    <button type="button" onClick={() => (item.taskId ? openTask(item.taskId) : item.robot && openRobot(item.robot))} title={title(item)}
      aria-current={selected || undefined}
      className={cn('flex w-full min-w-0 items-center gap-1 rounded-md px-1.5 py-1 text-left text-xs ring-inset', toneClass[scheduleItemTone(item)],
        selected ? 'ring-2 ring-brand-500' : 'ring-1')}>
      <span className="shrink-0 font-semibold tabular-nums">{format.time(item.at)}</span>
      <span className="shrink-0 [&>svg]:size-3.5" aria-hidden>
        {item.type === 'charging' ? <BatteryCharging /> : item.type === 'maintenance' ? <Wrench /> : <ServiceIcon service={item.service} />}
      </span>
      <span className="truncate">{compact ? kind : weekLabel}</span>
    </button>
  )
}

/** Schedule: tasks of the user's services, charging sessions and maintenance windows by day, week or month. */
export default function SchedulePage() {
  const { t } = useTranslation()
  const format = useFormat()
  const localize = useLocalize()
  const services = useAllowedServices()
  const serviceName = useServiceName()
  const canFleet = useCan('fleet.view')
  const robots = useFleetRobots(canFleet).data
  const zones = useCatalog().data?.zones
  const [params, setParams] = useSearchParams()
  const [view, setView] = useState<View>('day')
  const [anchor, setAnchor] = useState(() => Date.now())
  const service = params.get('service') ?? ''
  const robot = params.get('robot') ?? ''
  const zone = params.get('zone') ?? ''
  const [start, end] = useMemo(() => range(view, anchor), [view, anchor])
  const schedule = useSchedule({ start, end, service, robot, zone })
  const set = (key: string, value: string) => {
    const next = new URLSearchParams(params)
    if (value) next.set(key, value)
    else next.delete(key)
    setParams(next, { replace: true })
  }
  const items = schedule.data?.items ?? []
  const byDay = useMemo(() => {
    const map = new Map<string, ScheduleItem[]>()
    for (const i of items) map.set(format.dayKey(i.at), [...(map.get(format.dayKey(i.at)) ?? []), i])
    return map
  }, [items, format])
  const today = format.dayKey(Date.now())
  const title = view === 'day' ? format.date(start) : format.dateRange(start, end - 1)

  return (
    <>
      <PageHeader title={t('schedule.title')} subtitle={t('schedule.subtitle')} actions={<CreateTaskButton className={createTaskInHeader} />} />
      <Card className="p-4 sm:p-5">
        <div className="mb-4 flex flex-wrap items-center gap-3">
          <FilterTabs<View> value={view} onChange={setView} label={t('schedule.view')}
            items={(['day', 'week', 'month'] as const).map((v) => ({ value: v, label: t(`schedule.${v}`) }))} />
          <div className="flex items-center gap-1">
            <IconButton label={t('schedule.previous')} onClick={() => setAnchor((a) => shift(view, a, -1))}><ChevronLeft className="size-5" /></IconButton>
            <Button variant="ghost" size="sm" onClick={() => setAnchor(Date.now())}>{t('schedule.today')}</Button>
            <IconButton label={t('schedule.next')} onClick={() => setAnchor((a) => shift(view, a, 1))}><ChevronRight className="size-5" /></IconButton>
          </div>
          <p className="font-bold text-slate-900">{title}</p>
          <div className="ml-auto flex flex-wrap gap-2">
            <Select value={service} onChange={(e) => set('service', e.target.value)} aria-label={t('filters.service')} compact className="w-40">
              <option value="">{t('filters.allServices')}</option>
              {services.map((s) => <option key={s} value={s}>{serviceName(s)}</option>)}
            </Select>
            {canFleet && (
              <Select value={robot} onChange={(e) => set('robot', e.target.value)} aria-label={t('filters.robot')} compact className="w-36">
                <option value="">{t('filters.allRobots')}</option>
                {(robots ?? []).map((r) => <option key={r.name} value={r.name}>{r.name}</option>)}
              </Select>
            )}
            <Select value={zone} onChange={(e) => set('zone', e.target.value)} aria-label={t('filters.zone')} compact className="w-40">
              <option value="">{t('filters.allZones')}</option>
              {(zones ?? []).filter((z) => z.allowed).map((z) => <option key={z.id} value={z.id}>{localize(z.name)}</option>)}
            </Select>
          </div>
        </div>

        {schedule.isPending && <Skeleton className="h-80" />}
        {schedule.isError && <ErrorState onRetry={() => void schedule.refetch()} />}
        {schedule.data && view === 'day' && <ScheduleTimeline items={items} />}
        {schedule.data && view !== 'day' && (
          <div className="overflow-x-auto">
            <div className={cn('grid gap-px overflow-hidden rounded-xl bg-slate-200 ring-1 ring-slate-200', view === 'week' ? 'min-w-[840px] grid-cols-7' : 'min-w-[840px] grid-cols-7')}>
              {view === 'month' && Array.from({ length: (new Date(start).getDay() + 6) % 7 }, (_, i) => <div key={`pad-${i}`} className="bg-slate-50" />)}
              {days(start, end).map((d) => {
                const key = format.dayKey(d)
                const list = byDay.get(key) ?? []
                // A crowded day shows its first items and a "+N more" link to that day's list.
                const limit = view === 'week' ? 6 : 3
                const openDay = () => { setView('day'); setAnchor(d) }
                return (
                  <div key={key} className={cn('flex min-h-28 min-w-0 flex-col gap-1 p-1.5', key === today ? 'bg-brand-50/60' : 'bg-surface', view === 'week' && 'min-h-80')}>
                    <button type="button" onClick={openDay} aria-current={key === today ? 'date' : undefined}
                      className={cn('self-start rounded-md px-1.5 text-xs font-semibold', key === today ? 'bg-brand-100 text-brand-700 ring-1 ring-brand-200' : 'text-slate-600 hover:bg-slate-100')}>
                      {view === 'week' ? `${t(`schedule.weekday.${new Date(d).getDay()}`)} ${new Date(d).getDate()}` : new Date(d).getDate()}
                    </button>
                    {list.slice(0, limit).map((i) => <Chip key={i.id} item={i} compact={view === 'month'} />)}
                    {list.length > limit && (
                      <button type="button" onClick={openDay} className="self-start rounded-md px-1.5 text-xs font-semibold text-brand-700 hover:underline">
                        {t('schedule.more', { count: list.length - limit })}
                      </button>
                    )}
                  </div>
                )
              })}
            </div>
          </div>
        )}
        {schedule.data && items.length === 0 && view !== 'day' && (
          <p className="mt-4 flex items-center gap-2 text-sm text-slate-500"><CalendarDays className="size-4" />{t('empty.noSchedule')}</p>
        )}
      </Card>
    </>
  )
}
