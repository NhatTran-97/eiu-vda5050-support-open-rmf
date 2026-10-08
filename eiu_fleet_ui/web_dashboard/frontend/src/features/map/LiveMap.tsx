import { createElement, useMemo, useState, type ReactNode } from 'react'
import { renderToStaticMarkup } from 'react-dom/server'
import { useTranslation } from 'react-i18next'
import { useFleetRobots } from '../../api/queries'
import type { RobotStatus } from '../../api/types'
import { useServiceIcon } from '../../domain/services'
import { robotStatusColor } from '../../domain/status'
import { cn } from '../../lib/cn'
import { useUi } from '../../lib/ui'
import { useLive } from '../../realtime/store'
import { FleetMap } from './FleetMap'
import { blueprintRobotIcon } from './icons'
import type { MarkerLook } from './RobotMarker'

const LEGEND = ['IDLE', 'NAVIGATING', 'EXECUTING', 'CHARGING', 'PAUSED', 'MAINTENANCE', 'OFFLINE', 'ERROR'] as const

/**
 * Live operations map (dark technical blueprint) with every robot the user may see: the service is the marker's
 * icon, the status its ring. A click selects a robot (white ring, its route drawn as the active one) and opens its
 * drawer. The selected robot's pickup and destination are pinned; `chargers` adds the charging stations.
 */
export function LiveMap({ className, filter, colors = robotStatusColor, chargers, fill }: {
  className?: string
  /** Take the height given by `className` (see FleetMap `fill`). */
  fill?: boolean
  filter?: (service: string | null) => boolean
  colors?: Record<RobotStatus, string>
  chargers?: boolean
}) {
  const robots = useFleetRobots().data
  const liveNames = useLive((s) => Object.keys(s.robots).sort().join('\n'))
  const selected = useUi((s) => s.robot)
  const openRobot = useUi((s) => s.openRobot)
  const iconOf = useServiceIcon()
  const { t } = useTranslation()

  const shown = useMemo(() => (robots ?? []).filter((r) => !filter || filter(r.serviceType)), [robots, filter])
  const names = useMemo(() => {
    const live = new Set(liveNames.split('\n').filter(Boolean))
    return shown.map((r) => r.name).filter((n) => live.has(n))
  }, [shown, liveNames])
  const looks = useMemo(() => {
    const out: Record<string, MarkerLook> = {}
    for (const r of shown) {
      const svg = renderToStaticMarkup(createElement(iconOf(r.serviceType), { size: 17, strokeWidth: 2.25, color: '#ffffff' }))
      const isSelected = r.name === selected
      out[r.name] = {
        icon: blueprintRobotIcon({
          ring: colors[r.status], serviceSvg: svg, selected: isSelected,
          warning: r.health === 'warning' && r.status !== 'PAUSED' && r.status !== 'MAINTENANCE',
          alert: r.status === 'ERROR' || (r.health === 'critical' && r.status !== 'OFFLINE'),
        }),
        labelClass: isSelected ? 'map-label map-label--robot map-label--robot-selected' : 'map-label map-label--robot',
        title: `${r.name} · ${t(`robotStatus.${r.status}`)} · ${Math.round(r.battery)}%`,
        onSelect: () => openRobot(r.name),
      }
    }
    return out
  }, [shown, selected, iconOf, openRobot, colors, t])
  const task = shown.find((r) => r.name === selected)?.task
  const ends = task?.kind === 'delivery' ? [task.pickupId, task.dropoffId] : task?.kind === 'patrol' ? [task.stops[0], task.stops[task.stops.length - 1]] : []

  return <FleetMap fitFloor fill={fill} variant="blueprint" selectedRobot={selected} robotNames={names} looks={looks} showChargers={chargers}
    pickupId={ends[0]} dropoffId={ends[1]} className={className} />
}

/** Every map legend: items flow left to right and wrap only when the line is full; it never shrinks, so in a card of
 * fixed height the map gives up the room its rows need. */
const LEGEND_LIST = 'flex shrink-0 flex-wrap items-center gap-x-3.5 gap-y-2 text-meta'

/** Full legend of the live map in one list: robot states, then routes, destination, charging stations and warning.
 * `after` adds trailing items, e.g. a "Less" button. */
export function MapLegend({ className = 'text-slate-600', colors = robotStatusColor, after }: { className?: string; colors?: Record<RobotStatus, string>; after?: ReactNode }) {
  const { t } = useTranslation()
  return (
    <ul className={cn(LEGEND_LIST, className)}>
      {LEGEND.map((s) => (
        <li key={s} className="flex items-center gap-1.5">
          <span className="size-2.5 rounded-sm" style={{ background: colors[s] }} aria-hidden />{t(`robotStatus.${s}`)}
        </li>
      ))}
      <li className="flex items-center gap-1.5"><span className="h-1 w-5 rounded-full bg-[#3b82f6]" aria-hidden />{t('overview.legend.route')}</li>
      <li className="flex items-center gap-1.5">
        <span className="w-5 border-t-2 border-dashed border-[rgba(59,130,246,0.6)]" aria-hidden />{t('overview.legend.remaining')}
      </li>
      <li className="flex items-center gap-1.5"><span className="size-2.5 rounded-full bg-[#22c55e]" aria-hidden />{t('overview.legend.destination')}</li>
      <li className="flex items-center gap-1.5">
        <span className="flex size-3.5 items-center justify-center rounded-[3px] border border-[#4d7c0f] bg-[#1e3326]" aria-hidden>
          <span className="block size-1.5 rounded-[1px] bg-[#84cc16]" />
        </span>{t('overview.legend.charger')}
      </li>
      <li className="flex items-center gap-1.5"><span className="size-2 rounded-full bg-[#f59e0b] ring-2 ring-(--bp-bg)" aria-hidden />{t('overview.legend.warning')}</li>
      {after}
    </ul>
  )
}

const COMPACT = ['IDLE', 'NAVIGATING', 'EXECUTING', 'CHARGING'] as const

/** Short legend for the Overview: the common states, warning, route and destination; "More" opens the full legend. */
export function CompactMapLegend({ className }: { className?: string }) {
  const { t } = useTranslation()
  const [open, setOpen] = useState(false)
  if (open) {
    return (
      <MapLegend className={className}
        after={<li><button type="button" aria-expanded onClick={() => setOpen(false)} className="text-meta font-medium text-ops-blue hover:underline">{t('overview.legend.less')}</button></li>} />
    )
  }
  return (
    <ul className={cn(LEGEND_LIST, className)}>
      {COMPACT.map((s) => (
        <li key={s} className="flex items-center gap-1.5"><span className="size-2.5 rounded-sm" style={{ background: robotStatusColor[s] }} aria-hidden />{t(`robotStatus.${s}`)}</li>
      ))}
      <li className="flex items-center gap-1.5"><span className="size-2 rounded-full bg-[#f59e0b] ring-2 ring-(--bp-bg)" aria-hidden />{t('overview.legend.warning')}</li>
      <li className="flex items-center gap-1.5"><span className="h-1 w-5 rounded-full bg-[#3b82f6]" aria-hidden />{t('overview.legend.routeShort')}</li>
      <li className="flex items-center gap-1.5"><span className="size-2.5 rounded-full bg-[#22c55e]" aria-hidden />{t('overview.legend.destination')}</li>
      <li><button type="button" aria-expanded={false} onClick={() => setOpen(true)} className="text-meta font-medium text-ops-blue hover:underline">{t('overview.legend.more')}</button></li>
    </ul>
  )
}
