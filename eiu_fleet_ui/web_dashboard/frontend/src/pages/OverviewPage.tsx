import { Activity, Bot, CalendarDays, CircleCheck, ListTodo, Lock, Maximize2, Radar, TriangleAlert } from 'lucide-react'
import { useCallback, useEffect, useRef, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { Link } from 'react-router'
import { useFleetRobots, useOperationsOverview } from '../api/queries'
import type { RobotGroup } from '../api/types'
import { ErrorState, EmptyState, Skeleton } from '../components/ui/States'
import { useAllowedServices, useCan, useIsAdmin } from '../domain/access'
import { ServiceIcon, useServiceName } from '../domain/services'
import { CompactMapLegend, LiveMap } from '../features/map/LiveMap'
import { AttentionList, ServiceTile } from '../features/overview/OverviewParts'
import { Kpi, LiveDot, OpsCard, OpsCardHeader, OpsChip, OpsLink, SegmentBar, SignalCount, opsGroupColor } from '../features/overview/ops'
import { ScheduleTimeline } from '../features/overview/ScheduleTimeline'
import { TaskActivity } from '../features/overview/TaskActivity'
import { cn } from '../lib/cn'
import { useFormat } from '../lib/format'
import { useNow } from '../lib/useNow'

const GROUPS: RobotGroup[] = ['active', 'idle', 'charging', 'paused', 'maintenance', 'error', 'offline']
const GROUP_OF: Record<string, RobotGroup> = {
  NAVIGATING: 'active', EXECUTING: 'active', IDLE: 'idle', CHARGING: 'charging', PAUSED: 'paused', MAINTENANCE: 'maintenance', ERROR: 'error', OFFLINE: 'offline',
}

/** Service filter of the map: only the services the user may operate. */
function ServiceFilter({ value, onChange }: { value: string; onChange: (v: string) => void }) {
  const { t } = useTranslation()
  const services = useAllowedServices()
  const name = useServiceName()
  if (services.length < 2) return null
  const item = (v: string, label: string, icon?: React.ReactNode) => (
    <button key={v} type="button" role="radio" aria-checked={value === v} onClick={() => onChange(v)}
      className={cn('flex h-8 items-center gap-1.5 rounded-lg px-2.5 text-control font-medium transition-colors',
        value === v ? 'bg-ops-blue-soft text-ops-blue' : 'text-ops-muted hover:bg-ops-subtle hover:text-ops-text')}>
      {icon}{label}
    </button>
  )
  return (
    <div role="radiogroup" aria-label={t('filters.service')} className="flex flex-wrap gap-1 rounded-lg border border-ops-border p-0.5">
      {item('', t('filters.all'))}
      {services.map((s) => item(s, name(s), <ServiceIcon service={s} className="size-3.5" />))}
    </div>
  )
}

const ATTENTION_ROWS = 4
/** Two columns, about 70 / 30 with at least 400 px on the right. Stacked below 1280 px. */
const ROW = 'grid grid-cols-1 gap-4 xl:grid-cols-[minmax(0,7fr)_minmax(400px,3fr)]'

/**
 * Robot Operations Overview: the operator's starting point. Header and four KPIs, then two columns: live operations
 * above robot services on the left, needs attention above today's schedule on the right, and today's activity across
 * the page. The right column takes the height of the left one and splits it about 58 / 42 between attention and the
 * schedule, which scroll inside when they hold more. Below 1280 px the blocks stack as live operations, needs attention,
 * robot services, today's schedule, today's activity.
 */
export default function OverviewPage() {
  const { t } = useTranslation()
  const format = useFormat()
  const overview = useOperationsOverview()
  const canFleet = useCan('fleet.view')
  const robots = useFleetRobots(canFleet)
  const services = useAllowedServices()
  const admin = useIsAdmin()
  const now = useNow(1000)
  const [service, setService] = useState('')
  const mapFilter = useCallback((s: string | null) => !service || s === service, [service])
  const o = overview.data
  const loading = overview.isPending

  const list = robots.data ?? []
  const groups = Object.fromEntries(GROUPS.map((g) => [g, list.filter((r) => GROUP_OF[r.status] === g).length])) as Record<RobotGroup, number>
  const online = o?.robots.online ?? 0
  const total = o?.robots.total ?? 0
  // Robots available = robots that can take a new task now: online and IDLE, as the backend counts them
  // (`overview.robots.available`). IDLE excludes charging, executing or navigating a task, paused, maintenance,
  // error and offline robots, so availability is never the same as being online.
  const available = o?.robots.available ?? 0
  // The schedule preview opens at the first item that is not over yet: what happens next matters most here.
  const scheduleList = useRef<HTMLDivElement>(null)
  const scheduleKey = (o?.schedule ?? []).map((i) => `${i.id}:${i.status}`).join('|')
  useEffect(() => {
    const list = scheduleList.current
    const next = list?.querySelector<HTMLElement>('li:not([data-status="completed"]):not([data-status="cancelled"]):not([data-status="failed"])')
    if (list && next) list.scrollTop = next.offsetTop - list.offsetTop
  }, [scheduleKey])
  const updated = overview.dataUpdatedAt ? Math.max(0, Math.round((now - overview.dataUpdatedAt) / 1000)) : null

  if (overview.isError) {
    return <div className="pt-6"><OpsCard><ErrorState onRetry={() => void overview.refetch()} /></OpsCard></div>
  }
  if (o && services.length === 0 && !admin) {
    return <div className="pt-6"><OpsCard><EmptyState icon={<Lock />} title={t('empty.noServices.title')} body={t('empty.noServices.body')} /></OpsCard></div>
  }

  const liveCard = (
    <OpsCard className="order-1 flex min-w-0 flex-col gap-3 p-4 xl:order-none">
      <OpsCardHeader icon={Radar} title={t('overview.live')}
        meta={<><LiveDot />{t('overview.robotsOnMap', { count: list.length })}</>}
        action={<>
          <ServiceFilter value={service} onChange={setService} />
          <Link to="/live-operations" className="flex items-center gap-1 text-control font-medium text-ops-blue hover:underline"><Maximize2 className="size-4" />{t('overview.openLive')}</Link>
        </>} />
      {canFleet
        ? <LiveMap filter={mapFilter} chargers className="max-h-[440px] min-h-[220px] w-full" />
        : <EmptyState icon={<Lock />} title={t('empty.noFleetView')} />}
      <CompactMapLegend className="text-ops-muted" />
    </OpsCard>
  )

  const servicesSection = (
    <section aria-labelledby="robot-services" className="order-3 flex min-w-0 flex-col gap-2.5 xl:order-none">
      <div className="flex items-center gap-3">
        <h2 id="robot-services" className="text-section font-semibold text-ops-text">{t('overview.byService')}</h2>
        <OpsLink to="/fleet">{t('overview.openFleet')}</OpsLink>
      </div>
      {loading ? <Skeleton className="h-36" /> : (
        <div className="grid flex-1 gap-3 sm:grid-cols-2 md:grid-cols-3">
          {o?.services.map((card) => <ServiceTile key={card.id} card={card} />)}
        </div>
      )}
    </section>
  )

  const activityCard = (
    <OpsCard className="p-4">
      <OpsCardHeader icon={Activity} title={t('overview.activityToday')} className="mb-3" action={<OpsLink to="/analytics">{t('nav.analytics')}</OpsLink>} />
      <TaskActivity />
    </OpsCard>
  )

  const alerts = (o?.alerts ?? []).slice(0, ATTENTION_ROWS)
  // Without alerts the attention card keeps its natural height and the schedule takes the rest of the column.
  const attentionCard = (
    <OpsCard className={cn('order-2 flex flex-col p-4 xl:order-none xl:min-h-0', alerts.length > 0 && 'xl:basis-0 xl:grow-58')}>
      <OpsCardHeader icon={TriangleAlert} title={t('overview.attention')}
        meta={o && o.attention.open > 0 && <OpsChip tone={o.attention.critical > 0 ? 'red' : 'amber'}>{t('overview.openCount', { count: o.attention.open })}</OpsChip>}
        action={o && o.attention.open > 0 ? <OpsLink to="/notifications">{t('common.viewAll')}</OpsLink> : undefined} className="mb-3" />
      {loading ? <Skeleton className="h-48" /> : (
        <div className="-mx-1 px-1 xl:min-h-0 xl:flex-1 xl:overflow-y-auto"><AttentionList alerts={alerts} /></div>
      )}
    </OpsCard>
  )

  const scheduleCard = (
    <OpsCard className="order-4 flex min-h-[200px] flex-col p-4 xl:order-none xl:basis-0 xl:grow-42">
      <OpsCardHeader icon={CalendarDays} title={t('overview.today')} meta={format.date(now)} className="mb-1"
        action={<OpsLink to="/schedule">{t('common.viewAll')}</OpsLink>} />
      {loading ? <Skeleton className="h-40" /> : (
        // The list fills the card and scrolls inside it, so the card follows the row's height instead of its items.
        <div className="relative min-h-[120px] flex-1">
          <div ref={scheduleList} className="absolute inset-0 -mx-1 overflow-y-auto px-1"><ScheduleTimeline items={o?.schedule ?? []} /></div>
        </div>
      )}
    </OpsCard>
  )

  return (
    <div className="flex flex-col gap-4 pt-5 text-ops-text">
      <header className="flex flex-wrap items-end gap-x-6 gap-y-2">
        <div className="min-w-0 basis-full md:basis-0 md:flex-1">
          <h1 className="text-page-title font-semibold tracking-tight text-ops-text">{t('overview.title')}</h1>
          <p className="mt-1 text-card-title text-ops-muted">{t('overview.subtitle')}</p>
        </div>
        <span className="flex items-center gap-2 text-meta text-ops-muted" role="status">
          <LiveDot />{updated === null ? t('overview.connecting') : t('overview.updated', { time: format.time(overview.dataUpdatedAt), s: updated })}
        </span>
      </header>

      <div className="grid grid-cols-1 gap-4 sm:grid-cols-2 xl:grid-cols-4">
        <Kpi icon={Bot} tone="blue" label={t('overview.kpi.online')} loading={loading} value={online} unit={`/ ${total}`} to={canFleet ? '/fleet' : undefined}
          line={o && <>{t('overview.kpi.onlineCount', { count: online })} · <SignalCount value={total - online} tone="amber">{t('overview.kpi.offlineCount', { count: total - online })}</SignalCount></>}
          extra={canFleet && list.length > 0 && <SegmentBar parts={GROUPS.map((g) => ({ key: g, value: groups[g], color: opsGroupColor[g], label: t(`robotGroup.${g}`) }))} />} />
        <Kpi icon={ListTodo} tone="cyan" label={t('overview.kpi.tasks')} loading={loading} value={o?.tasks.active ?? 0} to="/tasks"
          line={o && t('overview.kpi.activeLine', { executing: o.tasks.executing, scheduled: o.tasks.scheduled })}
          extra={o && <p className="text-meta text-ops-muted">{t('overview.kpi.tasksLine', { completed: o.tasks.completedToday })}</p>} />
        <Kpi icon={CircleCheck} tone="green" label={t('overview.kpi.ready')} loading={loading}
          value={available} unit={`/ ${total}`} to={canFleet ? '/fleet?status=idle' : undefined}
          line={o && (canFleet ? t('overview.kpi.readyDetail', { charging: groups.charging, busy: groups.active + groups.paused }) : t('overview.kpi.readyHint'))} />
        <Kpi icon={TriangleAlert} tone="amber"
          label={t('overview.kpi.attention')} loading={loading} value={o?.attention.open ?? 0} to="/notifications"
          alert={o && o.attention.open > 0 ? (o.attention.critical > 0 ? 'red' : 'amber') : undefined}
          line={o && (o.attention.open
            ? <><SignalCount value={o.attention.critical} tone="red">{t('overview.kpi.criticalCount', { count: o.attention.critical })}</SignalCount> · <SignalCount value={o.attention.unacked} tone="amber">{t('overview.kpi.unackedCount', { count: o.attention.unacked })}</SignalCount></>
            : t('empty.noAlerts'))} />
      </div>

      {/* Below 1280 px both columns dissolve into the grid (display: contents) and `order` interleaves their blocks. */}
      <div className={ROW}>
        <div className="contents xl:flex xl:min-w-0 xl:flex-col xl:gap-4">
          {liveCard}
          {servicesSection}
        </div>
        {/* The right column adds no height of its own: its blocks are laid out in an absolute box over the row. */}
        <div className="contents xl:relative xl:block">
          <div className="contents xl:absolute xl:inset-0 xl:flex xl:flex-col xl:gap-4">
            {attentionCard}
            {scheduleCard}
          </div>
        </div>
      </div>

      {/* Today's activity across the page. */}
      {activityCard}
    </div>
  )
}
