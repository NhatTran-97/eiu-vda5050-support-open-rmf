import { BatteryCharging, Bot, Truck } from 'lucide-react'
import { useEffect, type ReactNode } from 'react'
import { useTranslation } from 'react-i18next'
import { Link, useLocation } from 'react-router'
import { useFleetRobots, useRegistration, useSystem } from '../../api/queries'
import type { AdapterSystem, FleetRegistry, FleetRobot } from '../../api/types'
import { Card, CardHeader } from '../../components/ui/Card'
import { EmptyState, ErrorState, Skeleton } from '../../components/ui/States'
import { ModeChip } from '../operations/RobotControls'
import { cn } from '../../lib/cn'
import { duration } from '../../lib/duration'

const LIMIT_UNITS: Record<string, string> = {
  linear_speed: 'm/s', linear_acceleration: 'm/s²', angular_speed: 'rad/s', angular_acceleration: 'rad/s²', footprint_radius: 'm', tolerance: '',
}

function Block({ title, children, className }: { title: string; children: ReactNode; className?: string }) {
  return (
    <section className={cn('rounded-xl bg-slate-50 px-4 py-3.5', className)}>
      <h3 className="mb-2 text-card-title font-semibold text-slate-800">{title}</h3>
      {children}
    </section>
  )
}

function FleetCard({ fleet, robots, adapter }: { fleet: FleetRegistry; robots: FleetRobot[]; adapter?: AdapterSystem }) {
  const { t } = useTranslation()
  const live = new Map(robots.map((r) => [r.name, r]))
  const status = adapter?.status ?? 'waiting'
  const tone = status === 'ok' ? 'bg-emerald-50 text-emerald-700' : ['critical', 'silent', 'absent'].includes(status) ? 'bg-red-50 text-red-700' : 'bg-amber-50 text-amber-700'
  return (
    <Card id={encodeURIComponent(fleet.fleet)} className="scroll-mt-24 p-4 sm:p-5">
      <CardHeader icon={<Truck />} title={fleet.fleet} subtitle={fleet.series} className="mb-4"
        action={<span className={cn('rounded-full px-2.5 py-1 text-xs font-semibold', tone)}>{t(`ops.adapterStatus.${status}`)}</span>} />
      {/* From lg two independent columns (general, robots, integration | limits, chargers): each block keeps its own
          height and the blocks of a column follow each other without gaps. Below lg the columns dissolve
          (display: contents) and `order` keeps the reading order general, limits, robots, chargers, integration. */}
      <div className="flex flex-col gap-3 lg:grid lg:grid-cols-2 lg:items-start">
        <div className="contents lg:flex lg:flex-col lg:gap-3">
          <Block title={t('admin.fleets.general')} className="order-1 lg:order-none">
            <dl className="grid grid-cols-[auto_1fr] gap-x-6 gap-y-1.5 text-sm">
              <dt className="text-slate-500">{t('admin.robots.adapterNode')}</dt><dd className="font-medium break-all text-slate-900">{fleet.adapter_node ?? '—'}</dd>
              <dt className="text-slate-500">{t('admin.robots.interface')}</dt><dd className="font-medium text-slate-900">{fleet.interface ? `VDA5050 · ${fleet.interface}` : '—'}</dd>
              <dt className="text-slate-500">{t('admin.robots.type')}</dt><dd className="text-slate-900">{fleet.series ?? '—'}</dd>
            </dl>
          </Block>
          <Block title={t('admin.fleets.robots', { count: fleet.robots.filter((r) => !r.retired).length })} className="order-3 lg:order-none">
            <ul className="flex flex-col gap-1.5">
              {fleet.robots.filter((r) => !r.retired).map((r) => {
                const robot = live.get(r.name)
                return (
                  <li key={r.name} className="flex items-center gap-2 text-sm">
                    <Bot className="size-4 text-slate-400" />
                    <Link to={`/fleet/${encodeURIComponent(r.name)}`} className="font-semibold text-slate-900 hover:underline">{r.name}</Link>
                    <span className="truncate text-slate-500">{r.manufacturer} / {r.serial}</span>
                    <span className="ml-auto">{robot ? <ModeChip robot={robot} /> : <span className="text-xs text-slate-500">{t('admin.fleets.noState')}</span>}</span>
                  </li>
                )
              })}
            </ul>
          </Block>
          {adapter && adapter.reported_ago_s !== null && (
            <Block title={t('admin.fleets.integration')} className="order-5 lg:order-none">
              <dl className="grid grid-cols-[auto_1fr] gap-x-6 gap-y-1.5 text-sm">
                <dt className="text-slate-500">MQTT</dt><dd className={cn('font-medium', adapter.mqtt.connected ? 'text-emerald-700' : 'text-red-700')}>{adapter.mqtt.connected ? t('ops.system.mqttOk') : t('ops.system.mqttLost')}</dd>
                <dt className="text-slate-500">{t('admin.fleets.lastReport')}</dt><dd className="text-slate-600">{t('ops.system.reported', { ago: duration(adapter.reported_ago_s), interval: duration(adapter.interval_s) })}</dd>
                <dt className="text-slate-500">{t('admin.fleets.robotsOnline')}</dt>
                <dd className={cn('font-medium tabular-nums', adapter.robots.online < adapter.robots.registered ? 'text-amber-700' : 'text-emerald-700')}>{adapter.robots.online} / {adapter.robots.registered}</dd>
              </dl>
            </Block>
          )}
        </div>
        <div className="contents lg:flex lg:flex-col lg:gap-3">
          <Block title={t('admin.fleets.limits')} className="order-2 lg:order-none">
            <dl className="grid grid-cols-[auto_1fr] gap-x-6 gap-y-1.5 text-sm">
              {Object.entries(fleet.limits ?? {}).map(([key, value]) => (
                <div key={key} className="contents">
                  <dt className="text-slate-500">{t(`admin.fleets.limit.${key}`, { defaultValue: key })}</dt>
                  <dd className="font-medium text-slate-900 tabular-nums">{value} {LIMIT_UNITS[key] ?? ''}</dd>
                </div>
              ))}
              {Object.keys(fleet.limits ?? {}).length === 0 && <dd className="text-slate-500">—</dd>}
            </dl>
          </Block>
          <Block title={t('admin.fleets.chargers')} className="order-4 lg:order-none">
            <dl className="grid grid-cols-[auto_1fr] items-center gap-x-6 gap-y-1.5 text-sm">
              {fleet.chargers.map((c) => (
                <div key={c.name} className="contents">
                  <dt className="flex items-center gap-2 font-medium text-slate-900"><BatteryCharging className="size-4 text-slate-400" aria-hidden />{c.name}</dt>
                  <dd className={c.used_by ? 'font-medium text-slate-900' : 'text-slate-500'}>
                    {c.used_by ? (c.used_by_removed ? t('admin.fleets.reservedFor', { robot: c.used_by }) : c.used_by) : t('admin.fleets.free')}
                  </dd>
                </div>
              ))}
            </dl>
          </Block>
        </div>
      </div>
      <p className="mt-3 text-xs text-slate-500">{t('admin.fleets.configHint')}</p>
    </Card>
  )
}

/** Fleets as objects of their own: adapter, interface, limits, robots, chargers and integration. */
export function FleetsPanel() {
  const { t } = useTranslation()
  const registration = useRegistration()
  const robots = useFleetRobots().data ?? []
  const adapters = useSystem().data?.adapters ?? []
  const { hash } = useLocation()

  useEffect(() => {
    if (hash && registration.data) document.getElementById(hash.slice(1))?.scrollIntoView({ behavior: 'smooth' })
  }, [hash, registration.data])

  return (
    <>
      {registration.isPending && <Skeleton className="h-80 rounded-2xl" />}
      {registration.isError && <Card><ErrorState onRetry={() => void registration.refetch()} /></Card>}
      {registration.data?.fleets.length === 0 && <Card><EmptyState icon={<Truck />} title={t('admin.fleets.empty')} /></Card>}
      <div className="flex flex-col gap-4 lg:gap-6">
        {registration.data?.fleets.map((f) => (
          <FleetCard key={f.fleet} fleet={f} robots={robots.filter((r) => r.fleet === f.fleet)}
            adapter={adapters.find((a) => a.fleet === f.fleet || a.node === f.adapter_node)} />
        ))}
      </div>
    </>
  )
}
