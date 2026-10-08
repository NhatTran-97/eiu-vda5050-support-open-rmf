import { useTranslation } from 'react-i18next'
import { useCatalog } from '../../api/queries'
import type { FleetRobot, TaskKind } from '../../api/types'
import { Facts } from '../../components/ui/Drawer'
import { useServiceMap } from '../../domain/services'
import { useLocalize } from '../../lib/i18nText'
import { useTaskPlace } from '../tasks/taskText'

/** Telemetry keys reported as a percentage. */
const PERCENT = new Set(['water_tank', 'waste_tank', 'brush_health', 'coverage'])

/** Rows that describe the robot whatever it is doing; the other rows belong to its current task. */
const PERSISTENT = new Set(['payload', 'compartment', 'water_tank', 'waste_tank', 'brush_health', 'cleaning_mode', 'camera'])

/** Rows of each category: telemetry keys the robot may report, or values of its current task. */
const ROWS: Record<TaskKind, string[]> = {
  delivery: ['payload', 'compartment', 'pickup', 'dropoff', 'qr_pin', 'route'],
  clean: ['water_tank', 'waste_tank', 'brush_health', 'cleaning_mode', 'coverage', 'area'],
  patrol: ['patrol_route', 'checkpoint', 'rounds', 'camera', 'zone'],
}

/** Section of the robot views specific to its service; a value the robot does not report shows "Not reported". Without a
 * task it is one "No active task" line and the persistent rows only. */
export function ServiceTelemetry({ robot }: { robot: FleetRobot }) {
  const { t } = useTranslation()
  const localize = useLocalize()
  const services = useServiceMap()
  const catalog = useCatalog().data
  const { text, place } = useTaskPlace()
  const kind = services.get(robot.serviceType ?? '')?.category
  if (!kind) return null
  const task = robot.task
  const fromTask: Record<string, string | null> = {
    pickup: task?.kind === 'delivery' ? place(task.pickupId) : null,
    dropoff: task?.kind === 'delivery' ? place(task.dropoffId) : null,
    route: task ? text(task) : null,
    area: task?.kind === 'clean' ? text(task) : null,
    patrol_route: task?.kind === 'patrol' ? text(task) : null,
    rounds: task?.kind === 'patrol' ? `${task.roundsDone} / ${task.rounds}` : null,
    zone: task?.zones.length ? task.zones.map((z) => localize(catalog?.zones.find((x) => x.id === z)?.name ?? { vi: z, en: z })).join(', ') : null,
  }
  const value = (key: string) => {
    const v = robot.telemetry[key]
    if (v !== undefined) {
      if (typeof v === 'boolean') return t(v ? 'common.yes' : 'common.no')
      if (typeof v === 'number' && PERCENT.has(key)) return `${Math.round(v)}%`
      return t(`telemetry.value.${String(v)}`, { defaultValue: t(`taskForm.option.itemType.${String(v)}`, { defaultValue: String(v) }) })
    }
    if (fromTask[key]) return fromTask[key]
    return <span className="font-normal text-slate-400">{t(key in fromTask ? 'telemetry.noTask' : 'telemetry.notReported')}</span>
  }
  if (!task) {
    return (
      <div className="flex flex-col gap-3">
        <p className="text-sm text-slate-500">{t('telemetry.idle')}</p>
        <Facts rows={ROWS[kind].filter((key) => PERSISTENT.has(key)).map((key) => [t(`telemetry.${key}`), value(key)])} />
      </div>
    )
  }
  return <Facts rows={ROWS[kind].map((key) => [t(`telemetry.${key}`), value(key)])} />
}
