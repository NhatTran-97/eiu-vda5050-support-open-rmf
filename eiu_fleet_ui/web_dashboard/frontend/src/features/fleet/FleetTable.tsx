import { useTranslation } from 'react-i18next'
import { Link } from 'react-router'
import type { FleetRobot } from '../../api/types'
import { DataTable, type Column } from '../../components/ui/DataTable'
import { ServiceTag } from '../../domain/services'
import { BatteryIndicator, RobotStatusBadge } from '../../domain/status'
import { cn } from '../../lib/cn'
import { useNow } from '../../lib/useNow'
import { useUi } from '../../lib/ui'
import { taskCode } from '../tasks/taskText'
import { runtimeText, useRobotLocation } from './RobotDetailsDrawer'

/** Robots as a table; a row opens the robot drawer. */
export function FleetTable({ robots, empty }: { robots: FleetRobot[]; empty: React.ReactNode }) {
  const { t } = useTranslation()
  const now = useNow(10_000)
  const location = useRobotLocation()
  const openRobot = useUi((s) => s.openRobot)
  const openTask = useUi((s) => s.openTask)
  const selected = useUi((s) => s.robot)
  const columns: Column<FleetRobot>[] = [
    { key: 'robot', header: t('fleet.col.robot'), cell: (r) => <span className="font-semibold whitespace-nowrap text-slate-900">{r.name}</span> },
    { key: 'type', header: t('fleet.col.type'), cell: (r) => <span className="whitespace-nowrap"><ServiceTag service={r.serviceType} /></span> },
    { key: 'status', header: t('fleet.col.status'), cell: (r) => <RobotStatusBadge status={r.status} /> },
    { key: 'battery', header: t('fleet.col.battery'), cell: (r) => <BatteryIndicator value={r.battery} charging={r.status === 'CHARGING'} /> },
    { key: 'task', header: t('fleet.col.task'), cell: (r) => (r.task
      ? <button type="button" className="font-semibold text-brand-700 hover:underline" onClick={(e) => { e.stopPropagation(); openTask(r.task!.id) }}>{taskCode(r.task.id, r.task.service)}</button>
      : <span className="text-slate-400">—</span>) },
    { key: 'location', header: t('fleet.col.location'), cell: (r) => <span className="whitespace-nowrap">{location(r)}</span> },
    { key: 'runtime', header: t('fleet.col.runtime'), cell: (r) => <span className="tabular-nums">{runtimeText(r, now)}</span> },
    { key: 'connection', header: t('fleet.col.connection'), cell: (r) => (
      <span className={cn('flex items-center gap-1.5 whitespace-nowrap', r.connection === 'online' ? 'text-slate-600' : 'font-medium text-red-700')}>
        <span className={cn('size-2 rounded-full', r.connection === 'online' ? 'bg-emerald-500' : 'bg-red-500')} />{t(`connection.${r.connection}`)}
      </span>
    ) },
    { key: 'actions', header: <span className="sr-only">{t('fleet.col.actions')}</span>, cell: (r) => (
      <Link to={`/fleet/${encodeURIComponent(r.name)}`} onClick={(e) => e.stopPropagation()} className="text-sm font-semibold whitespace-nowrap text-brand-700 hover:underline">{t('fleet.details')}</Link>
    ) },
  ]
  return <DataTable columns={columns} rows={robots} rowKey={(r) => r.name} onRow={(r) => openRobot(r.name)} selected={selected} empty={empty} minWidth={1000} />
}
