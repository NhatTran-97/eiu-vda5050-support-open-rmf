import { ExternalLink, ListChecks } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { useNavigate } from 'react-router'
import { useFleetRobots, useLocations } from '../../api/queries'
import type { FleetRobot } from '../../api/types'
import { Button } from '../../components/ui/Button'
import { Drawer, Facts } from '../../components/ui/Drawer'
import { EmptyState } from '../../components/ui/States'
import { ServiceTag, useServiceName } from '../../domain/services'
import { BatteryIndicator, HealthBadge, RobotStatusBadge } from '../../domain/status'
import { duration } from '../../lib/duration'
import { useFormat } from '../../lib/format'
import { useLocalize } from '../../lib/i18nText'
import { useNow } from '../../lib/useNow'
import { useUi } from '../../lib/ui'
import { taskCode, useTaskPlace } from '../tasks/taskText'
import { ServiceTelemetry } from './ServiceTelemetry'

export function useRobotLocation() {
  const localize = useLocalize()
  const locations = useLocations().data
  return (robot: Pick<FleetRobot, 'locationId'>) => {
    const loc = locations?.find((l) => l.id === robot.locationId)
    return loc ? localize(loc.name) : '—'
  }
}

export function runtimeText(robot: FleetRobot, now: number): string {
  return robot.onlineSince ? duration(Math.max(0, (now - robot.onlineSince) / 1000)) : '—'
}

/** Facts every robot has, whatever its service: operational state first (status, battery, task, location, connection,
 * health), then supporting metadata in a quieter block (runtime, last update, type, fleet). */
export function RobotFacts({ robot }: { robot: FleetRobot }) {
  const { t } = useTranslation()
  const format = useFormat()
  const now = useNow(10_000)
  const location = useRobotLocation()
  const { text } = useTaskPlace()
  const openTask = useUi((s) => s.openTask)
  return (
    <div className="flex flex-col gap-3">
      <Facts rows={[
        [t('fleet.col.status'), <RobotStatusBadge status={robot.status} />],
        [t('fleet.col.battery'), <BatteryIndicator value={robot.battery} charging={robot.status === 'CHARGING'} />],
        [t('fleet.col.task'), robot.task
          ? <button type="button" className="text-left font-semibold text-brand-700 hover:underline" onClick={() => openTask(robot.task!.id)}>
              {taskCode(robot.task.id, robot.task.service)} · <span className="font-normal text-slate-700">{text(robot.task)}</span>
            </button>
          : t('fleet.noTask')],
        ...(robot.task?.etaAt ? [[t('fleet.eta'), format.timeLeft(robot.task.etaAt, now)] as [string, string]] : []),
        [t('fleet.col.location'), location(robot)],
        [t('fleet.col.connection'), <span className={robot.connection === 'online' ? 'text-emerald-700' : 'text-red-700'}>{t(`connection.${robot.connection}`)}</span>],
        [t('fleet.health'), <HealthBadge health={robot.health} />],
      ]} />
      <Facts quiet className="border-t border-slate-100 pt-3" rows={[
        [t('fleet.col.runtime'), runtimeText(robot, now)],
        [t('fleet.lastUpdate'), robot.lastUpdateAt ? format.time(robot.lastUpdateAt) : '—'],
        [t('fleet.col.type'), <ServiceTag service={robot.serviceType} />],
        [t('fleet.col.fleet'), robot.fleet],
      ]} />
    </div>
  )
}

/** Robot clicked on the map or in a list: shared facts, then the section of its service. */
export function RobotDetailsDrawer() {
  const { t } = useTranslation()
  const navigate = useNavigate()
  const name = useRobotName()
  const openRobot = useUi((s) => s.openRobot)
  const openTask = useUi((s) => s.openTask)
  const serviceName = useServiceName()
  const robots = useFleetRobots(name !== null).data
  const robot = robots?.find((r) => r.name === name)
  return (
    <Drawer open={name !== null} onClose={() => openRobot(null)} title={name ?? ''}
      subtitle={robot && t('fleet.robotOf', { service: serviceName(robot.serviceType) })}
      footer={robot && (
        <>
          {robot.task && <Button variant="secondary" size="sm" icon={<ListChecks className="size-4" />} onClick={() => openTask(robot.task!.id)}>{t('fleet.viewTask')}</Button>}
          <Button size="sm" icon={<ExternalLink className="size-4" />} onClick={() => { openRobot(null); navigate(`/fleet/${encodeURIComponent(robot.name)}`) }}>{t('fleet.viewRobot')}</Button>
        </>
      )}>
      {robots && !robot && <EmptyState icon={<ListChecks />} title={t('fleet.robotGone')} />}
      {robot && (
        <div className="flex flex-col gap-6">
          <RobotFacts robot={robot} />
          <section>
            <h3 className="mb-3 text-sm font-bold text-slate-900">{t('fleet.serviceSection', { service: serviceName(robot.serviceType) })}</h3>
            <ServiceTelemetry robot={robot} />
          </section>
        </div>
      )}
    </Drawer>
  )
}

function useRobotName() {
  return useUi((s) => s.robot)
}
