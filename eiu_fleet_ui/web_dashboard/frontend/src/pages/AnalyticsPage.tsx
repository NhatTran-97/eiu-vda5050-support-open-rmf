import { Activity, BatteryMedium, BellRing, Download, ListChecks, MapPin, type LucideIcon } from 'lucide-react'
import { useState, type ReactNode } from 'react'
import { useTranslation } from 'react-i18next'
import { useAnalytics } from '../api/queries'
import type { AnalyticsReport } from '../api/types'
import { Button } from '../components/ui/Button'
import { Card, CardHeader } from '../components/ui/Card'
import { ErrorState, Skeleton } from '../components/ui/States'
import { Tabs } from '../components/ui/Tabs'
import { Bars, ChartFrame, StackedColumns, type Series } from '../features/charts/Charts'
import { PageHeader } from '../layout/PageHeader'
import { useLocalize } from '../lib/i18nText'
import { useUtilizationSeries } from './RobotPage'
import { useCatalog, useFleetRobots } from '../api/queries'
import { Select } from '../components/ui/Field'
import { useAllowedServices, useCan } from '../domain/access'
import { ServiceIcon, useServiceName } from '../domain/services'
import type { ServiceFigures } from '../api/types'

const RANGES = ['1', '7', '30'] as const

function Kpi({ label, value, hint }: { label: string; value: ReactNode; hint?: string }) {
  return (
    <Card className="flex flex-col px-4 py-3.5">
      <p className="text-card-title font-medium text-ops-muted">{label}</p>
      <p className="mt-2 text-kpi-secondary font-semibold text-ops-text tabular-nums">{value}</p>
      {hint && <p className="mt-1.5 text-meta text-ops-muted">{hint}</p>}
    </Card>
  )
}

function Panel({ icon: Icon, title, subtitle, children }: { icon: LucideIcon; title: string; subtitle?: string; children: ReactNode }) {
  return (
    <Card className="p-4 sm:p-5">
      <CardHeader icon={<Icon />} title={title} subtitle={subtitle} className="mb-3" />
      {children}
    </Card>
  )
}

const dayLabel = (day: string) => day.slice(5).replace('-', '/')

function exportCsv(report: AnalyticsReport) {
  const lines = [['day', 'created', 'completed', 'failed', 'cancelled', 'active_h', 'idle_h', 'charging_h', 'offline_h', 'alerts_critical', 'alerts_warning', 'alerts_info']]
  report.tasksPerDay.forEach((d, i) => {
    const u = report.utilization[i]
    const a = report.alertsPerDay[i]
    lines.push([d.day, d.created, d.completed, d.failed, d.cancelled, u?.active, u?.idle, u?.charging, u?.offline, a?.critical, a?.warning, a?.info].map(String))
  })
  const url = URL.createObjectURL(new Blob([lines.map((l) => l.join(',')).join('\n')], { type: 'text/csv' }))
  const link = Object.assign(document.createElement('a'), { href: url, download: `eiu-analytics-${report.days}d.csv` })
  link.click()
  URL.revokeObjectURL(url)
}

/** Figures of one service; a figure without data reads "No data yet". */
function ServiceSection({ figures }: { figures: ServiceFigures }) {
  const { t } = useTranslation()
  const name = useServiceName()
  const minutes = (v: number | null | undefined) => (v === null || v === undefined ? '—' : t('admin.analytics.minutes', { v }))
  const none = <span className="block text-sm leading-snug font-normal text-slate-500">{t('analytics.svc.noData')}</span>
  const rows: [string, ReactNode][] = figures.category === 'delivery'
    ? [[t('analytics.svc.deliveries'), figures.completed], [t('analytics.svc.avgDelivery'), minutes(figures.avgDurationMin)], [t('analytics.svc.distance'), figures.distanceKm ?? none]]
    : figures.category === 'clean'
      ? [[t('analytics.svc.areaCleaned'), figures.areaM2 ?? none], [t('analytics.svc.cleaningDuration'), minutes(figures.avgDurationMin)], [t('analytics.svc.coverage'), figures.coveragePct ?? none]]
      : [[t('analytics.svc.rounds'), figures.rounds ?? 0], [t('analytics.svc.zones'), figures.zonesCovered ?? 0], [t('analytics.svc.avgRound'), minutes(figures.avgRoundMin)]]
  return (
    <Card className="p-4 sm:p-5">
      <CardHeader icon={<ServiceIcon service={figures.id} className="size-5" />} title={name(figures.id)} subtitle={t('analytics.svc.created', { count: figures.created })} className="mb-3" />
      <dl className="grid grid-cols-3 gap-3">
        {rows.map(([label, value]) => <div key={label}><dt className="text-meta text-slate-500">{label}</dt><dd className="mt-1 text-xl font-bold text-slate-900">{value}</dd></div>)}
      </dl>
    </Card>
  )
}

/** Delivery performance, robot utilization and alerts over 1, 7 or 30 days, from the task records and the robot history. */
export default function AnalyticsPage() {
  const { t } = useTranslation()
  const localize = useLocalize()
  const [range, setRange] = useState<(typeof RANGES)[number]>('7')
  const [service, setService] = useState('')
  const [robot, setRobot] = useState('')
  const [zone, setZone] = useState('')
  const services = useAllowedServices()
  const serviceName = useServiceName()
  const robots = useFleetRobots(useCan('fleet.view')).data
  const zones = useCatalog().data?.zones
  const report = useAnalytics({ days: Number(range), service, robot, zone })
  const utilSeries = useUtilizationSeries()
  const outcomeSeries: Series[] = [
    { key: 'completed', label: t('admin.analytics.completed'), color: 'var(--color-status-success)' },
    { key: 'failed', label: t('admin.analytics.failed'), color: 'var(--color-status-critical)' },
    { key: 'cancelled', label: t('admin.analytics.cancelled'), color: 'var(--color-status-cancelled)' },
  ]
  const alertSeries: Series[] = [
    { key: 'warning', label: t('alerts.severity.warning'), color: 'var(--color-status-warning)' },
    { key: 'info', label: t('alerts.severity.info'), color: 'var(--color-status-active)' },
    { key: 'critical', label: t('alerts.severity.critical'), color: 'var(--color-status-critical)' },
  ]
  const r = report.data
  const minutes = (v: number | null) => (v === null ? '—' : t('admin.analytics.minutes', { v }))
  const pct = (v: number | null) => (v === null ? '—' : `${v}%`)

  return (
    <>
      <PageHeader title={t('admin.analytics.title')} subtitle={t('admin.analytics.subtitle')}
        actions={r && <Button variant="secondary" icon={<Download className="size-4" />} onClick={() => exportCsv(r)}>{t('admin.analytics.export')}</Button>} />
      <div className="mb-4 flex flex-wrap items-center gap-2 rounded-2xl bg-surface p-1.5 shadow-card ring-1 ring-slate-900/5 lg:mb-6">
        <Tabs value={range} onChange={setRange} label={t('admin.analytics.range')}
          items={RANGES.map((v) => ({ value: v, label: t(`admin.analytics.ranges.${v}`) }))} />
        <div className="ml-auto flex flex-wrap gap-2 p-1">
          <Select value={service} onChange={(e) => setService(e.target.value)} aria-label={t('filters.service')} compact className="w-40">
            <option value="">{t('filters.allServices')}</option>
            {services.map((s) => <option key={s} value={s}>{serviceName(s)}</option>)}
          </Select>
          <Select value={robot} onChange={(e) => setRobot(e.target.value)} aria-label={t('filters.robot')} compact className="w-36">
            <option value="">{t('filters.allRobots')}</option>
            {(robots ?? []).map((r) => <option key={r.name} value={r.name}>{r.name}</option>)}
          </Select>
          <Select value={zone} onChange={(e) => setZone(e.target.value)} aria-label={t('filters.zone')} compact className="w-40">
            <option value="">{t('filters.allZones')}</option>
            {(zones ?? []).filter((z) => z.allowed).map((z) => <option key={z.id} value={z.id}>{localize(z.name)}</option>)}
          </Select>
        </div>
      </div>
      {report.isPending && <Skeleton className="h-96 rounded-2xl" />}
      {report.isError && <Card><ErrorState onRetry={() => void report.refetch()} /></Card>}
      {r && (
        <div className="flex flex-col gap-4 lg:gap-6">
          <div className="grid grid-cols-2 gap-3 sm:grid-cols-3 xl:grid-cols-6 lg:gap-4">
            <Kpi label={t('admin.analytics.kpi.created')} value={r.kpis.created} hint={t('admin.analytics.kpi.outcome', r.kpis)} />
            <Kpi label={t('admin.analytics.kpi.success')} value={pct(r.kpis.successRate)} hint={t('admin.analytics.kpi.successHint')} />
            <Kpi label={t('admin.analytics.kpi.duration')} value={minutes(r.kpis.avgDurationMin)} hint={t('admin.analytics.kpi.durationHint')} />
            <Kpi label={t('admin.analytics.kpi.wait')} value={minutes(r.kpis.avgWaitMin)} hint={t('admin.analytics.kpi.waitHint')} />
            <Kpi label={t('admin.analytics.kpi.utilization')} value={pct(r.kpis.utilizationPct)} hint={t('admin.analytics.kpi.utilizationHint')} />
            <Kpi label={t('admin.analytics.kpi.alerts')} value={r.kpis.alerts}
              hint={t('admin.analytics.kpi.alertsHint', { critical: r.alertsPerDay.reduce((n, d) => n + d.critical, 0) })} />
          </div>
          {r.byService.length > 0 && (
            <div className="grid gap-4 md:grid-cols-2 xl:grid-cols-3 lg:gap-6">
              {r.byService.map((f) => <ServiceSection key={f.id} figures={f} />)}
            </div>
          )}
          {/* The two charts of a row share its height (grid stretch, content top-aligned); stacked below lg at natural height. */}
          <div className="grid gap-4 lg:grid-cols-2 lg:gap-6">
            <Panel icon={ListChecks} title={t('admin.analytics.tasksPerDay')}>
              <ChartFrame legend={outcomeSeries} table={{ head: [t('admin.util.day'), t('admin.analytics.createdShort'), ...outcomeSeries.map((s) => s.label)],
                rows: r.tasksPerDay.map((d) => [dayLabel(d.day), d.created, d.completed, d.failed, d.cancelled]) }}>
                <StackedColumns series={outcomeSeries} empty={t('admin.analytics.noTasks')}
                  data={r.tasksPerDay.map((d) => ({ label: dayLabel(d.day), values: { completed: d.completed, failed: d.failed, cancelled: d.cancelled } }))} />
              </ChartFrame>
            </Panel>
            <Panel icon={Activity} title={t('admin.analytics.utilization')} subtitle={t('admin.util.hint', { s: r.sampleS })}>
              <ChartFrame legend={utilSeries} table={{ head: [t('admin.util.day'), ...utilSeries.map((s) => s.label)],
                rows: r.utilization.map((d) => [dayLabel(d.day), d.active, d.charging, d.idle, d.offline]) }}>
                <StackedColumns series={utilSeries} format={(v) => `${Math.round(v * 10) / 10} h`} empty={t('admin.util.empty')}
                  data={r.utilization.map((d) => ({ label: dayLabel(d.day), values: { active: d.active, charging: d.charging, idle: d.idle, offline: d.offline } }))} />
              </ChartFrame>
            </Panel>
            <Panel icon={MapPin} title={t('admin.analytics.places')} subtitle={t('admin.analytics.placesHint')}>
              {r.topPlaces.length === 0 ? <p className="text-sm text-slate-500">{t('admin.analytics.noTasks')}</p> : (
                <ChartFrame table={{ head: [t('admin.analytics.place'), t('admin.analytics.count')], rows: r.topPlaces.map((p) => [localize(p.name), p.count]) }}>
                  <Bars color="var(--color-chart-blue)" items={r.topPlaces.map((p) => ({ label: localize(p.name), value: p.count }))} />
                </ChartFrame>
              )}
            </Panel>
            <Panel icon={BatteryMedium} title={t('admin.analytics.battery')} subtitle={t('admin.analytics.batteryHint')}>
              <ChartFrame table={{ head: [t('ops.robots.battery'), t('admin.analytics.robots')], rows: r.battery.map((b) => [`${b.from}–${b.to}%`, b.robots]) }}>
                <Bars color="var(--color-chart-blue)" items={r.battery.map((b) => ({ label: `${b.from}–${b.to}%`, value: b.robots }))} />
              </ChartFrame>
            </Panel>
            <Panel icon={BellRing} title={t('admin.analytics.alertsPerDay')}>
              <ChartFrame legend={alertSeries} table={{ head: [t('admin.util.day'), ...alertSeries.map((s) => s.label)],
                rows: r.alertsPerDay.map((d) => [dayLabel(d.day), d.warning, d.info, d.critical]) }}>
                <StackedColumns series={alertSeries} empty={t('alerts.noHistory')}
                  data={r.alertsPerDay.map((d) => ({ label: dayLabel(d.day), values: { warning: d.warning, info: d.info, critical: d.critical } }))} />
              </ChartFrame>
            </Panel>
            <Panel icon={BellRing} title={t('admin.analytics.alertCodes')}>
              {r.alertCodes.length === 0 ? <p className="text-sm text-slate-500">{t('alerts.noHistory')}</p> : (
                <Bars color="var(--color-chart-blue)" items={r.alertCodes.map((c) => ({ label: t(`admin.analytics.alertCode.${c.code}`, { defaultValue: c.code }), value: c.count }))} />
              )}
            </Panel>
          </div>
        </div>
      )}
    </>
  )
}
