import { Bot, Search, Truck } from 'lucide-react'
import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import { useSearchParams } from 'react-router'
import { useFleetRobots } from '../api/queries'
import type { RobotStatus } from '../api/types'
import { Card } from '../components/ui/Card'
import { Input } from '../components/ui/Field'
import { FilterTabs } from '../components/ui/FilterTabs'
import { EmptyState, ErrorState, Skeleton } from '../components/ui/States'
import { Tabs } from '../components/ui/Tabs'
import { useAllowedServices, useCan, useSeesAllServices } from '../domain/access'
import { ServiceIcon, useServiceName } from '../domain/services'
import { FleetsPanel } from '../features/fleet/FleetsPanel'
import { FleetTable } from '../features/fleet/FleetTable'
import { PendingRobotsCard } from '../features/operations/Registration'
import { PageHeader } from '../layout/PageHeader'
import { fold } from '../lib/text'

/** Group names are written for use inside sentences; as filter labels they start with a capital letter. */
const sentenceCase = (text: string) => text.charAt(0).toUpperCase() + text.slice(1)

/** Status filter of the fleet table: one entry may cover several robot states. */
const STATUS_GROUPS: Record<string, RobotStatus[]> = {
  active: ['NAVIGATING', 'EXECUTING', 'PAUSED'],
  idle: ['IDLE'],
  charging: ['CHARGING'],
  maintenance: ['MAINTENANCE', 'ERROR'],
  offline: ['OFFLINE'],
}

function RobotsTab() {
  const { t } = useTranslation()
  const services = useAllowedServices()
  const seesAll = useSeesAllServices()
  const serviceName = useServiceName()
  const robots = useFleetRobots()
  const [params, setParams] = useSearchParams()
  const service = params.get('service') ?? ''
  const status = params.get('status') ?? ''
  const [q, setQ] = useState('')
  const set = (key: string, value: string) => {
    const next = new URLSearchParams(params)
    if (value) next.set(key, value)
    else next.delete(key)
    setParams(next, { replace: true })
  }
  const all = robots.data ?? []
  const inService = all.filter((r) => !service || (service === 'other' ? r.services.length === 0 : r.services.includes(service)))
  const shown = inService.filter((r) => (!status || STATUS_GROUPS[status].includes(r.status)) && (!q || fold(`${r.name} ${r.fleet}`).includes(fold(q))))
  return (
    <Card className="p-4 sm:p-5">
      <div className="mb-4 flex flex-col gap-3">
        <div className="flex flex-wrap items-center gap-3">
          <FilterTabs value={service} onChange={(v) => set('service', v)} label={t('filters.service')} muteZero
            items={[{ value: '', label: t('filters.all'), count: all.length }, ...services.map((s) => ({ value: s, label: serviceName(s), icon: <ServiceIcon service={s} className="size-3.5" />,
              count: all.filter((r) => r.services.includes(s)).length })),
            ...(seesAll && all.some((r) => r.services.length === 0) ? [{ value: 'other', label: t('service.other') }] : [])]} />
          <div className="relative ml-auto w-full sm:w-64">
            <Search className="pointer-events-none absolute top-1/2 left-3 size-4 -translate-y-1/2 text-slate-400" />
            <Input value={q} onChange={(e) => setQ(e.target.value)} placeholder={t('fleet.search')} aria-label={t('fleet.search')} className="h-10 pl-9" />
          </div>
        </div>
        <FilterTabs value={status} onChange={(v) => set('status', v)} label={t('filters.status')} muteZero
          items={[{ value: '', label: t('filters.all') }, ...Object.entries(STATUS_GROUPS).map(([g, list]) => ({ value: g, label: sentenceCase(t(`robotGroup.${g}`)),
            count: inService.filter((r) => list.includes(r.status)).length }))]} />
      </div>
      {robots.isPending && <Skeleton className="h-64" />}
      {robots.isError && <ErrorState onRetry={() => void robots.refetch()} />}
      {robots.data && <FleetTable robots={shown} empty={<EmptyState icon={<Bot />} title={all.length ? t('empty.noMatch') : t('empty.noRobots')}
        body={all.length ? undefined : t('empty.noRobotsBody')} />} />}
    </Card>
  )
}

/** Fleet: every robot the user may see, filtered by service and status; admins also manage fleets and registration. */
export default function FleetPage() {
  const { t } = useTranslation()
  const canManage = useCan('robots.manage')
  const [tab, setTab] = useState<'robots' | 'fleets'>('robots')
  return (
    <>
      <PageHeader title={t('fleet.title')} subtitle={t('fleet.subtitle')} />
      {canManage && (
        <Tabs<'robots' | 'fleets'> value={tab} onChange={setTab} label={t('fleet.title')} className="mb-4 rounded-2xl bg-surface p-1.5 shadow-card ring-1 ring-slate-900/5"
          items={[{ value: 'robots', label: t('fleet.tabRobots'), icon: <Bot className="size-4" /> }, { value: 'fleets', label: t('fleet.tabFleets'), icon: <Truck className="size-4" /> }]} />
      )}
      {tab === 'robots' || !canManage ? <RobotsTab /> : (
        <div className="flex flex-col gap-4 lg:gap-6">
          <PendingRobotsCard />
          <FleetsPanel />
        </div>
      )}
    </>
  )
}
