import { Activity, BatteryMedium, ChevronRight, ClipboardList, Cpu, ListChecks, Map as MapIcon, Package, Route } from 'lucide-react'
import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import { Link, useParams } from 'react-router'
import { useLevels, useLocations, useRegistration, useRobotDetail } from '../api/queries'
import type { RobotDetailView, RobotEvent } from '../api/types'
import { RobotIllustration } from '../assets/RobotArt'
import { Card, CardHeader } from '../components/ui/Card'
import { ErrorState, Skeleton } from '../components/ui/States'
import { useCan } from '../domain/access'
import { useServiceName } from '../domain/services'
import { useAuditText } from '../features/admin/Audit'
import { RobotDetail } from '../features/admin/RobotDetail'
import { ChartFrame, StackedColumns, TimeLine, type Series } from '../features/charts/Charts'
import { RobotFacts } from '../features/fleet/RobotDetailsDrawer'
import { ServiceTelemetry } from '../features/fleet/ServiceTelemetry'
import { MapCanvas } from '../features/operations/MapCanvas'
import { RobotActions, type RobotInfo } from '../features/operations/RobotControls'
import { PageHeader } from '../layout/PageHeader'
import { cn } from '../lib/cn'
import { duration } from '../lib/duration'
import { useFormat } from '../lib/format'
import { useLocalize } from '../lib/i18nText'
import { useUi } from '../lib/ui'

const RANGES = [1, 24, 168] as const

/** Technical keys an operator reads; the rest (adapter, interface, identity, charger, path) sits under "Advanced details". */
const OPERATIONAL_TECH = new Set(['rmf_mode', 'rmf_task', 'nearest_waypoint', 'nearest_distance_m', 'level', 'position', 'controls', 'paused', 'speed_limit_mps'])

export function useUtilizationSeries(): Series[] {
  const { t } = useTranslation()
  return [
    { key: 'active', label: t('admin.util.active'), color: 'var(--color-status-active)' },
    { key: 'charging', label: t('admin.util.charging'), color: 'var(--color-status-charging)' },
    { key: 'idle', label: t('admin.util.idle'), color: 'var(--color-status-idle)' },
    { key: 'offline', label: t('admin.util.offline'), color: 'var(--color-status-offline)' },
  ]
}

function TechValue({ name, value }: { name: string; value: string | number | boolean | null }) {
  const { t } = useTranslation()
  if (value === null || value === '' || value === undefined) return <>—</>
  if (typeof value === 'boolean') return <>{t(value ? 'admin.yes' : 'admin.no')}</>
  if (name.endsWith('_s') && typeof value === 'number') return <>{duration(value)}</>
  if (name.endsWith('_m') && typeof value === 'number') return <>{value} m</>
  if (name.endsWith('_mps') && typeof value === 'number') return <>{value === 0 ? t('ops.robots.speedNone') : `${value} m/s`}</>
  if (name === 'rmf_mode') return <>{t(`ops.mode.${value}`, { defaultValue: String(value) })}</>
  return <span className="break-all">{String(value)}</span>
}

function EventRow({ event }: { event: RobotEvent }) {
  const { t, i18n } = useTranslation()
  const format = useFormat()
  const audit = useAuditText()
  let title = event.code
  let detail = event.text ?? ''
  if (event.source === 'alert') {
    const p = event.params ?? {}
    title = event.code === 'adapter.attention' ? String(p.title ?? '') : t(`alerts.code.${event.code}.title`, { ...p, defaultValue: event.code })
    detail = event.closedAt ? t('admin.robotPage.closedAt', { at: format.time(event.closedAt) }) : t('admin.robotPage.ongoing')
  } else if (event.source === 'audit') {
    const x = audit({ id: 0, at: event.at, actor: event.actor ?? null, action: event.code, target: '', detail: event.text ?? '' })
    title = `${event.actor ?? t('admin.audit.system')} · ${x.action}`
    detail = x.detail
  } else {
    const key = `admin.robotPage.taskEvent.${event.code}`
    title = `#${event.deliveryId} · ${i18n.exists(key) ? t(key) : event.code}`
  }
  const tone = event.level === 'critical' ? 'bg-red-50 text-red-700' : event.level === 'warning' ? 'bg-amber-50 text-amber-700' : 'bg-slate-100 text-slate-600'
  return (
    <tr className="align-top">
      <td className="px-2 py-2 whitespace-nowrap text-slate-600">{format.dateTime(event.at)}</td>
      <td className="px-2 py-2"><span className={cn('rounded-full px-2 py-0.5 text-xs font-semibold', tone)}>{t(`alerts.severity.${event.level}`)}</span></td>
      <td className="px-2 py-2 text-slate-600">{t(`admin.robotPage.source.${event.source}`)}</td>
      <td className="px-2 py-2 font-medium text-slate-900">{title}</td>
      <td className="max-w-80 px-2 py-2 break-words text-slate-600">{detail || '—'}</td>
    </tr>
  )
}

function Details({ view, onConfigure }: { view: RobotDetailView; onConfigure: () => void }) {
  const { t } = useTranslation()
  const format = useFormat()
  const localize = useLocalize()
  const locations = useLocations().data
  const level = useLevels().data?.find((l) => l.id === view.robot.levelId)
  const series = useUtilizationSeries()
  const r = view.robot
  const place = (id: string) => { const l = locations?.find((x) => x.id === id); return l ? localize(l.name) : id }
  const tech = Object.entries(view.technical).filter(([k]) => OPERATIONAL_TECH.has(k))
  const advanced = Object.entries(view.technical).filter(([k]) => !OPERATIONAL_TECH.has(k))
  const techRows = (rows: [string, string | number | boolean | null][]) => rows.map(([k, v]) => (
    <div key={k} className="contents">
      <dt className="text-slate-500">{t(`admin.robotPage.tech.${k}`)}</dt>
      <dd className="text-slate-900"><TechValue name={k} value={v} /></dd>
    </div>
  ))
  const serviceName = useServiceName()
  const openTask = useUi((s) => s.openTask)
  const ageS = view.technical.fleet_state_age_s as number | null

  return (
    <div className="flex flex-col gap-4">
      {/* Top row: summary, service and location differ too much in content, so each keeps its own height. */}
      <div className="grid grid-cols-1 gap-4 lg:grid-cols-3 lg:items-start">
        <Card className="flex flex-col gap-4 p-4 sm:px-5 sm:py-4">
          <div className="flex items-center gap-4">
            <span className="flex size-16 shrink-0 items-center justify-center rounded-2xl bg-slate-50"><RobotIllustration className="h-12 w-auto" /></span>
            <div className="min-w-0">
              <p className="text-2xl font-bold text-slate-900">{r.name}</p>
              <p className="text-meta text-slate-600">{[view.technical.manufacturer, view.technical.interface && `VDA5050 · ${view.technical.interface}`].filter(Boolean).join(' · ') || r.fleet}</p>
              <p className="text-meta text-slate-500">{r.fleet}{ageS !== null && r.connection === 'online' ? ` · ${t('admin.robotPage.stateAge', { age: duration(ageS) })}` : ''}</p>
            </div>
          </div>
          <RobotFacts robot={r} />
          <div className="border-t border-slate-100 pt-3"><RobotActions robot={r} onConfigure={onConfigure} /></div>
        </Card>
        <Card className="p-4 sm:px-5 sm:py-4">
          <CardHeader icon={<Package />} title={t('fleet.serviceSection', { service: serviceName(r.serviceType) })} className="mb-3" />
          <ServiceTelemetry robot={r} />
        </Card>
        <Card className="flex flex-col gap-3 p-4 sm:px-5 sm:py-4">
          <CardHeader icon={<MapIcon />} title={t('admin.robotPage.location')} subtitle={view.technical.nearest_waypoint ? t('admin.robotPage.near', { waypoint: view.technical.nearest_waypoint, distance: view.technical.nearest_distance_m }) : undefined} />
          {level ? <MapCanvas level={level} className="max-h-64" /> : <Skeleton className="h-56" />}
        </Card>
      </div>

      {/* Middle and bottom rows: the cards of a row share its height (grid stretch, content top-aligned); stacked below lg. */}
      <div className="grid grid-cols-1 gap-4 lg:grid-cols-3">
        <Card className="p-4 sm:px-5 sm:py-4">
          <CardHeader icon={<Cpu />} title={t('admin.robotPage.technical')} className="mb-3" />
          <dl className="grid grid-cols-[auto_1fr] gap-x-5 gap-y-2 text-sm">{techRows(tech)}</dl>
          {advanced.length > 0 && (
            <details className="group mt-3 border-t border-slate-100 pt-3">
              <summary className="flex w-fit cursor-pointer list-none items-center gap-1.5 rounded-md text-sm font-semibold text-brand-700 hover:underline [&::-webkit-details-marker]:hidden">
                <ChevronRight className="size-4 transition-transform group-open:rotate-90" aria-hidden />{t('admin.robotPage.advanced')} ({advanced.length})
              </summary>
              <dl className="mt-3 grid grid-cols-[auto_1fr] gap-x-5 gap-y-2 text-sm">{techRows(advanced)}</dl>
            </details>
          )}
        </Card>
        <BatteryCard name={r.name} />
        <Card className="p-4 sm:px-5 sm:py-4">
          <CardHeader icon={<Activity />} title={t('admin.robotPage.utilization')} subtitle={t('admin.util.hint', { s: view.sampleS })} className="mb-3" />
          <ChartFrame legend={series} table={{ head: [t('admin.util.day'), ...series.map((s) => s.label)], rows: view.utilization.map((d) => [d.day.slice(5), d.active, d.charging, d.idle, d.offline]) }}>
            <StackedColumns series={series} format={(v) => `${Math.round(v * 10) / 10} h`} empty={t('admin.util.empty')}
              data={view.utilization.map((d) => ({ label: d.day.slice(5).replace('-', '/'), values: { active: d.active, charging: d.charging, idle: d.idle, offline: d.offline } }))} />
          </ChartFrame>
        </Card>
      </div>

      <div className="grid grid-cols-1 gap-4 lg:grid-cols-[minmax(0,1fr)_minmax(0,2fr)]">
        <Card className="p-4 sm:px-5 sm:py-4">
          <CardHeader icon={<ListChecks />} title={t('admin.robotPage.recentTasks')} className="mb-3" />
          {view.tasks.length === 0 ? <p className="text-sm text-slate-500">{t('ops.tasks.empty')}</p> : (
            <ul className="-mr-2 max-h-96 divide-y divide-slate-100 overflow-y-auto pr-2">
              {view.tasks.map((task) => (
                <li key={task.id} className="flex items-center gap-3 py-2 text-sm">
                  <Route className="size-4 shrink-0 text-slate-400" />
                  <span className="min-w-0 flex-1">
                    <button type="button" onClick={() => openTask(task.id)} className="font-semibold text-slate-900 hover:underline">#{task.id}</button>
                    <span className="block truncate text-slate-600">{task.kind === 'delivery' ? `${place(task.pickupId)} → ${place(task.dropoffId)}` : t(`kind.${task.kind}`)}</span>
                    <span className="text-xs text-slate-500">{format.dateTime(task.createdAt)}</span>
                  </span>
                  <span className={cn('text-xs font-semibold', task.status === 'failed' ? 'text-red-700' : task.status === 'completed' || task.status === 'cancelled' ? 'text-slate-500' : 'text-brand-700')}>{t(`status.${task.status}`)}</span>
                </li>
              ))}
            </ul>
          )}
        </Card>
        <Card className="p-4 sm:px-5 sm:py-4">
          <CardHeader icon={<ClipboardList />} title={t('admin.robotPage.events')} subtitle={t('admin.robotPage.eventsHint')} className="mb-3" />
          {view.events.length === 0 ? <p className="text-sm text-slate-500">{t('admin.robotPage.noEvents')}</p> : (
            <div className="-mx-2 max-h-96 overflow-auto">
              <table className="w-full min-w-[640px] text-sm">
                <thead className="sticky top-0 bg-surface"><tr>
                  {['time', 'level', 'source', 'event', 'detail'].map((h) => <th key={h} className="px-2 py-2 text-left text-xs font-semibold text-slate-500 uppercase">{t(`admin.robotPage.col.${h}`)}</th>)}
                </tr></thead>
                <tbody className="divide-y divide-slate-100">{view.events.map((e, i) => <EventRow key={`${e.source}-${e.at}-${i}`} event={e} />)}</tbody>
              </table>
            </div>
          )}
        </Card>
      </div>
    </div>
  )
}

function BatteryCard({ name }: { name: string }) {
  const { t } = useTranslation()
  const format = useFormat()
  const [hours, setHours] = useState<number>(24)
  const detail = useRobotDetail(name, hours)
  const points = detail.data?.battery ?? []
  return (
    <Card className="p-4 sm:px-5 sm:py-4">
      <CardHeader icon={<BatteryMedium />} title={t('admin.robotPage.battery')} className="mb-3"
        action={
          <select value={hours} onChange={(e) => setHours(Number(e.target.value))} aria-label={t('admin.robotPage.range')}
            className="h-9 rounded-lg border border-slate-200 bg-field px-2 text-sm text-slate-800">
            {RANGES.map((h) => <option key={h} value={h}>{t(`admin.robotPage.hours.${h}`)}</option>)}
          </select>
        } />
      <ChartFrame table={{ head: [t('admin.robotPage.col.time'), t('ops.robots.battery')], rows: points.map(([at, v]) => [format.dateTime(at), `${v}%`]) }}>
        <TimeLine points={points} color="var(--color-chart-blue)" max={100} unit="%" empty={t('admin.robotPage.noSamples')}
          formatTime={(ms) => (hours <= 1 ? format.time(ms) : `${format.shortDate(ms)} ${format.time(ms)}`)} />
      </ChartFrame>
    </Card>
  )
}

/** One robot: live state, mission, location, technical status, battery trend, utilization, tasks and events. */
export default function RobotDetailPage() {
  const { t } = useTranslation()
  const { name } = useParams()
  const detail = useRobotDetail(name, 24)
  const registration = useRegistration(useCan('robots.manage')).data
  const [configure, setConfigure] = useState(false)
  const fleet = registration?.fleets.find((f) => f.robots.some((r) => r.name === name))
  const reg = fleet?.robots.find((r) => r.name === name)
  const info: RobotInfo = { series: fleet?.series, interface: fleet?.interface, manufacturer: reg?.manufacturer, serial: reg?.serial, charger: reg?.charger, source: reg?.source }

  return (
    <>
      <PageHeader
        before={<nav aria-label={t('admin.robotPage.breadcrumb')} className="mb-2 flex items-center gap-1 text-control text-ops-muted">
          <Link to="/fleet" className="hover:text-ops-text">{t('nav.fleet')}</Link><ChevronRight className="size-4" /><span className="text-ops-text">{name}</span>
        </nav>}
        title={t('admin.robotPage.title', { robot: name })}
        subtitle={t('admin.robotPage.subtitle')}
      />
      {detail.isPending && <Skeleton className="h-96 rounded-2xl" />}
      {detail.isError && <Card><ErrorState onRetry={() => void detail.refetch()} /></Card>}
      {detail.data && <Details view={detail.data} onConfigure={() => setConfigure(true)} />}
      {configure && detail.data && <RobotDetail robot={detail.data.robot} info={info} fleet={fleet} onClose={() => setConfigure(false)} />}
    </>
  )
}
