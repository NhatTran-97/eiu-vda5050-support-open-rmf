import { ClipboardList, Hourglass, Search, Timer } from 'lucide-react'
import { useEffect, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { useSearchParams } from 'react-router'
import { useTasks } from '../api/queries'
import type { TaskState } from '../api/types'
import { Card } from '../components/ui/Card'
import { Input } from '../components/ui/Field'
import { FilterTabs } from '../components/ui/FilterTabs'
import { StatCard } from '../components/ui/StatCard'
import { EmptyState, ErrorState, Skeleton } from '../components/ui/States'
import { useAllowedServices } from '../domain/access'
import { ServiceIcon, useServiceName } from '../domain/services'
import { SignalCount } from '../features/overview/ops'
import { CreateTaskButton, createTaskInHeader } from '../features/tasks/CreateTaskDialog'
import { TaskTable } from '../features/tasks/TaskTable'
import { PageHeader } from '../layout/PageHeader'
import { useUi } from '../lib/ui'

const STATES: TaskState[] = ['SCHEDULED', 'QUEUED', 'ASSIGNED', 'EXECUTING', 'PAUSED', 'COMPLETED', 'CANCELLED', 'FAILED']

/** Tasks of every service the user may see, filtered by service and status; a row opens its drawer. */
export default function TasksPage() {
  const { t } = useTranslation()
  const services = useAllowedServices()
  const serviceName = useServiceName()
  const openTask = useUi((s) => s.openTask)
  const [params, setParams] = useSearchParams()
  const service = params.get('service') ?? ''
  const state = (params.get('state') ?? '') as TaskState | ''
  const mine = params.get('mine') === '1'
  const [q, setQ] = useState(params.get('q') ?? '')
  const [query, setQuery] = useState(q)
  useEffect(() => {
    const id = window.setTimeout(() => setQuery(q), 250)
    return () => window.clearTimeout(id)
  }, [q])
  useEffect(() => {
    const id = Number(params.get('task'))
    if (id) openTask(id)
  }, [params, openTask])
  const tasks = useTasks({ service, state, q: query, group: 'all', mine })
  const update = (key: string, value: string) => {
    const next = new URLSearchParams(params)
    if (value) next.set(key, value)
    else next.delete(key)
    next.delete('task')
    setParams(next, { replace: true })
  }
  const d = tasks.data

  return (
    <>
      <PageHeader title={t('tasks.title')} subtitle={t('tasks.subtitle')} actions={<CreateTaskButton className={createTaskInHeader} />} />
      <div className="mb-4 grid grid-cols-2 gap-3 xl:grid-cols-4 lg:mb-6 lg:gap-6">
        <StatCard icon={ClipboardList} label={t('tasks.kpi.active')} value={d?.summary.active ?? 0} loading={tasks.isPending}
          line={d && t('tasks.kpi.activeLine', { executing: d.counts.EXECUTING ?? 0, paused: d.counts.PAUSED ?? 0 })} />
        <StatCard icon={Hourglass} label={t('tasks.kpi.queued')} value={d?.summary.queued ?? 0} loading={tasks.isPending}
          tone={d && d.summary.queued > 0 ? 'warning' : 'brand'}
          line={d && (d.unassigned[0] ? t('tasks.kpi.waitingSince', { code: `#${d.unassigned[0].id}` }) : t('tasks.kpi.noneWaiting'))} />
        <StatCard icon={Timer} label={t('tasks.kpi.late')} value={d?.summary.late ?? 0} loading={tasks.isPending} tone={d && d.summary.late > 0 ? 'critical' : 'brand'}
          line={d && (d.late[0] ? t('tasks.kpi.lateOldest', { code: `#${d.late[0].id}` }) : t('tasks.kpi.noneLate'))} />
        <StatCard icon={ClipboardList} label={t('tasks.kpi.completedToday')} value={d?.summary.completedToday ?? 0} loading={tasks.isPending}
          tone="success" line={d && <>{d.summary.avgDurationMin !== null && <>{t('tasks.kpi.avg', { min: d.summary.avgDurationMin })} · </>}
            <SignalCount value={d.summary.failedToday} tone="red">{t('tasks.kpi.failedToday', { count: d.summary.failedToday })}</SignalCount></>} />
      </div>
      <Card className="p-4 sm:p-5">
        <div className="mb-4 flex flex-col gap-3.5">
          <div className="flex flex-wrap items-center gap-3">
            <span className="w-16 shrink-0 text-meta font-medium text-ops-muted">{t('filters.service')}</span>
            <FilterTabs value={service} onChange={(v) => update('service', v)} label={t('filters.service')}
              items={[{ value: '', label: t('filters.all') }, ...services.map((s) => ({ value: s, label: serviceName(s), icon: <ServiceIcon service={s} className="size-3.5" /> }))]} />
            <label className="ml-auto flex shrink-0 items-center gap-2 text-sm text-slate-600">
              <input type="checkbox" className="accent-brand-600" checked={mine} onChange={(e) => update('mine', e.target.checked ? '1' : '')} />
              {t('tasks.mineOnly')}
            </label>
            <div className="relative w-full sm:w-72">
              <Search className="pointer-events-none absolute top-1/2 left-3 size-4 -translate-y-1/2 text-slate-400" />
              <Input value={q} onChange={(e) => setQ(e.target.value)} placeholder={t('tasks.search')} aria-label={t('tasks.search')} className="h-10 pl-9" />
            </div>
          </div>
          <div className="flex items-center gap-3">
            <span className="w-16 shrink-0 text-meta font-medium text-ops-muted">{t('filters.status')}</span>
            <FilterTabs value={state} onChange={(v) => update('state', v)} label={t('filters.status')} muteZero className="min-w-0 flex-1"
              items={[{ value: '' as TaskState | '', label: t('filters.all') }, ...STATES.map((s) => ({ value: s as TaskState | '', label: t(`taskState.${s}`), count: d?.counts[s] }))]} />
          </div>
        </div>
        {tasks.isPending && <Skeleton className="h-64" />}
        {tasks.isError && <ErrorState onRetry={() => void tasks.refetch()} />}
        {d && <TaskTable tasks={d.items} empty={<EmptyState icon={<ClipboardList />} title={state || service || query ? t('empty.noMatch') : t('empty.noTasks')} />} />}
      </Card>
    </>
  )
}
