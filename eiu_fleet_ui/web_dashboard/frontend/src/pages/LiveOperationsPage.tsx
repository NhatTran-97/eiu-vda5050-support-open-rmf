import { BellRing, Bot, Lock } from 'lucide-react'
import { useCallback, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { useAlerts, useFleetRobots } from '../api/queries'
import { Card, CardHeader } from '../components/ui/Card'
import { FilterTabs } from '../components/ui/FilterTabs'
import { EmptyState, Skeleton } from '../components/ui/States'
import { useAllowedServices, useCan, useSeesAllServices } from '../domain/access'
import { ServiceIcon, useServiceName } from '../domain/services'
import { BatteryIndicator, RobotStatusBadge } from '../domain/status'
import { LiveMap, MapLegend } from '../features/map/LiveMap'
import { NeedsAttention } from '../features/overview/NeedsAttention'
import { LiveDot } from '../features/overview/ops'
import { CreateTaskButton, createTaskInHeader } from '../features/tasks/CreateTaskDialog'
import { PageHeader } from '../layout/PageHeader'
import { cn } from '../lib/cn'
import { useFormat } from '../lib/format'
import { useUi } from '../lib/ui'

/** Live Operations: the floor map with every robot the user sees, the robot list beside it and what needs attention. */
export default function LiveOperationsPage() {
  const { t } = useTranslation()
  const canFleet = useCan('fleet.view')
  const services = useAllowedServices()
  const seesAll = useSeesAllServices()
  const serviceName = useServiceName()
  const robots = useFleetRobots(canFleet)
  const format = useFormat()
  const alerts = useAlerts('open', canFleet)
  const selected = useUi((s) => s.robot)
  const openRobot = useUi((s) => s.openRobot)
  const [service, setService] = useState('')
  const filter = useCallback((s: string | null) => !service || s === service, [service])
  const shown = (robots.data ?? []).filter((r) => !service || r.services.includes(service))

  // Live status on the right of the header (from sm, where the top bar holds "Create task"): when the robot list last arrived.
  const live = (
    <span className="hidden items-center gap-2 text-meta text-ops-muted sm:flex" role="status">
      <LiveDot />{robots.dataUpdatedAt ? t('overview.updated', { time: format.time(robots.dataUpdatedAt) }) : t('overview.connecting')}
    </span>
  )
  const header = <PageHeader title={t('live.title')} subtitle={t('live.subtitle')} actions={<>{live}<CreateTaskButton className={createTaskInHeader} /></>} />
  if (!canFleet) return <>{header}<Card><EmptyState icon={<Lock />} title={t('empty.noFleetView')} /></Card></>

  // From 1280 px the page fills the viewport below the top bar (--topbar-h) down to the main padding (32 px): the map takes
  // the height left under the header and filters, and the right column matches it, its lists scrolling inside.
  return (
    <div className="flex flex-col xl:h-[calc(100dvh-var(--topbar-h)-2rem)] xl:min-h-[36rem]">
      {header}
      <FilterTabs value={service} onChange={setService} label={t('filters.service')} className="mb-4"
        items={[{ value: '', label: t('filters.all'), count: robots.data?.length }, ...services.map((s) => ({
          value: s, label: serviceName(s), icon: <ServiceIcon service={s} className="size-3.5" />,
          count: robots.data?.filter((r) => r.services.includes(s)).length,
        })), ...(seesAll && robots.data?.some((r) => r.services.length === 0) ? [{ value: 'other', label: t('service.other') }] : [])]} />
      <div className="grid items-start gap-4 lg:gap-6 xl:min-h-0 xl:flex-1 xl:grid-cols-[minmax(0,1fr)_380px] xl:grid-rows-[minmax(0,1fr)] xl:items-stretch">
        <Card className="flex flex-col gap-3 p-3 sm:p-4 xl:min-h-0">
          <LiveMap fill filter={service === 'other' ? (s) => s === null : filter} chargers className="h-[min(60dvh,560px)] min-h-[320px] w-full xl:h-auto xl:min-h-0 xl:flex-1" />
          <MapLegend className="px-1 text-slate-600" />
        </Card>
        <div className="flex flex-col gap-4 lg:gap-6 xl:min-h-0">
          <Card className="flex flex-col p-4 xl:max-h-[55%] xl:min-h-0">
            <CardHeader icon={<BellRing />} title={t('overview.attention')} className="mb-3" />
            {alerts.isPending ? <Skeleton className="h-32" /> : <div className="max-h-[40dvh] overflow-y-auto xl:max-h-none xl:min-h-0"><NeedsAttention alerts={alerts.data?.items ?? []} /></div>}
          </Card>
          <Card className="flex flex-col p-4 xl:min-h-0 xl:flex-1">
            <CardHeader icon={<Bot />} title={t('live.robots')} className="mb-2" />
            {robots.isPending && <Skeleton className="h-40" />}
            {robots.data && shown.length === 0 && <EmptyState icon={<Bot />} title={t('empty.noRobots')} />}
            <ul className="flex max-h-[40dvh] flex-col gap-0.5 overflow-y-auto xl:max-h-none xl:min-h-0 xl:flex-1">
              {shown.map((r) => (
                <li key={r.name}>
                  <button type="button" onClick={() => openRobot(r.name)}
                    className={cn('flex w-full items-center gap-3 rounded-xl px-2.5 py-1.5 text-left hover:bg-slate-50', selected === r.name && 'bg-brand-50 ring-1 ring-brand-200')}>
                    <span className="flex size-8 items-center justify-center rounded-lg bg-slate-100 text-slate-600"><ServiceIcon service={r.serviceType} /></span>
                    <span className="min-w-0 flex-1">
                      <span className="block text-body leading-tight font-semibold text-slate-900">{r.name}</span>
                      <span className="block truncate text-meta text-slate-500">{serviceName(r.serviceType)}</span>
                    </span>
                    <RobotStatusBadge status={r.status} />
                    <span className="hidden sm:block"><BatteryIndicator value={r.battery} /></span>
                  </button>
                </li>
              ))}
            </ul>
          </Card>
        </div>
      </div>
    </div>
  )
}
