import { MapPin, Pause, Play, Shuffle, X } from 'lucide-react'
import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import { useNavigate } from 'react-router'
import { useCatalog, useFleetRobots, useLocations, useTask, useTaskAction, type TaskAction } from '../../api/queries'
import type { TaskDetail } from '../../api/types'
import { Button } from '../../components/ui/Button'
import { Dialog } from '../../components/ui/Dialog'
import { Drawer, Facts } from '../../components/ui/Drawer'
import { Select } from '../../components/ui/Field'
import { ErrorState, Skeleton } from '../../components/ui/States'
import { ServiceTag, useServiceMap } from '../../domain/services'
import { TaskStatusBadge } from '../../domain/status'
import { useErrorText } from '../../lib/errors'
import { useFormat } from '../../lib/format'
import { useLocalize } from '../../lib/i18nText'
import { toast } from '../../lib/toast'
import { useUi } from '../../lib/ui'
import { useFieldLabel } from './DynamicTaskForm'
import { TaskTimeline } from './TaskTimeline'
import { taskCode } from './taskText'

function Parameters({ task }: { task: TaskDetail }) {
  const { t } = useTranslation()
  const localize = useLocalize()
  const label = useFieldLabel()
  const service = useServiceMap().get(task.service)
  const locations = useLocations().data
  const catalog = useCatalog().data
  const rows = (service?.taskFormSchema ?? []).filter((f) => f.type !== 'schedule' && task.parameters[f.key] !== undefined).map((f) => {
    const v = task.parameters[f.key]
    const name = (list: { id: string; name: { vi: string; en: string } }[] | undefined, id: string) => {
      const item = list?.find((x) => x.id === id)
      return item ? localize(item.name) : id
    }
    let text: string
    if (f.type === 'location') text = name(locations, String(v))
    else if (f.type === 'locations') text = (v as string[]).map((x) => name(locations, x)).join(' → ')
    else if (f.type === 'area') text = name(catalog?.areas, String(v))
    else if (f.type === 'route') text = name(catalog?.routes, String(v))
    else if (f.type === 'zone') text = name(catalog?.zones, String(v))
    else if (f.type === 'select') text = t(`taskForm.option.${f.key}.${v}`, { defaultValue: String(v) })
    else if (f.type === 'number') text = f.unit ? `${v} ${t(`taskForm.unit.${f.unit}`, { defaultValue: f.unit })}` : String(v)
    else text = String(v)
    return [label(f), text] as [string, string]
  })
  if (rows.length === 0) return null
  return (
    <section>
      <h3 className="mb-3 text-card-title font-semibold text-slate-900">{t('tasks.parameters')}</h3>
      <Facts rows={rows} />
    </section>
  )
}

function ReassignDialog({ task, onClose }: { task: TaskDetail; onClose: () => void }) {
  const { t } = useTranslation()
  const robots = useFleetRobots().data?.filter((r) => r.services.includes(task.service) && r.connection === 'online' && r.name !== task.robot?.name)
  const action = useTaskAction()
  const errorText = useErrorText()
  const openTask = useUi((s) => s.openTask)
  const [robot, setRobot] = useState('')
  return (
    <Dialog open onClose={onClose} title={t('tasks.reassignTitle', { code: taskCode(task.id, task.service) })}
      footer={<>
        <Button variant="ghost" onClick={onClose}>{t('common.cancel')}</Button>
        <Button disabled={!robot} loading={action.isPending} onClick={() => action.mutate({ id: task.id, action: 'reassign', robot }, {
          onSuccess: (next) => { toast.success(t('tasks.reassigned', { code: taskCode(next.id, next.service), robot })); onClose(); openTask(next.id) },
          onError: (e) => toast.error(errorText(e)),
        })}>{t('tasks.reassign')}</Button>
      </>}>
      <p className="mb-3 text-sm text-slate-600">{t('tasks.reassignBody')}</p>
      <Select value={robot} onChange={(e) => setRobot(e.target.value)} aria-label={t('taskForm.robot')}>
        <option value="">{t('taskForm.selectRobot')}</option>
        {(robots ?? []).map((r) => <option key={r.name} value={r.name}>{r.name} · {t(`robotStatus.${r.status}`)}</option>)}
      </Select>
    </Dialog>
  )
}

/** One task: status, who asked, which robot, its times, its parameters and what happened so far. */
export function TaskDetailsDrawer() {
  const { t } = useTranslation()
  const format = useFormat()
  const navigate = useNavigate()
  const id = useUi((s) => s.task)
  const openTask = useUi((s) => s.openTask)
  const openRobot = useUi((s) => s.openRobot)
  const task = useTask(id ?? undefined)
  const action = useTaskAction()
  const errorText = useErrorText()
  const [reassign, setReassign] = useState(false)
  const d = task.data
  const run = (kind: TaskAction) => d && action.mutate({ id: d.id, action: kind }, {
    onSuccess: () => toast.success(t(`tasks.actionSent.${kind}`)),
    onError: (e) => toast.error(errorText(e)),
  })
  const time = (ms: number | null) => (ms ? format.dateTime(ms) : '—')
  const hasActions = !!d && ((d.actions.track && !!d.robot) || d.actions.reassign || d.actions.pause || d.actions.resume || d.actions.cancel)

  return (
    <Drawer open={id !== null} onClose={() => openTask(null)}
      title={d ? taskCode(d.id, d.service) : t('tasks.details')}
      subtitle={d && <span className="flex flex-wrap items-center gap-2"><ServiceTag service={d.service} /><TaskStatusBadge state={d.state} /></span>}
      footer={d && hasActions && (
        <>
          {d.actions.track && d.robot && (
            <Button variant="secondary" size="sm" icon={<MapPin className="size-4" />} onClick={() => { openTask(null); openRobot(d.robot!.name); navigate('/live-operations') }}>
              {t('tasks.viewOnMap')}
            </Button>
          )}
          {d.actions.reassign && <Button variant="secondary" size="sm" icon={<Shuffle className="size-4" />} onClick={() => setReassign(true)}>{t('tasks.reassign')}</Button>}
          {d.actions.pause && <Button variant="secondary" size="sm" icon={<Pause className="size-4" />} loading={action.isPending} onClick={() => run('pause')}>{t('tasks.pause')}</Button>}
          {d.actions.resume && <Button size="sm" icon={<Play className="size-4" />} loading={action.isPending} onClick={() => run('resume')}>{t('tasks.resume')}</Button>}
          {d.actions.cancel && <Button variant="danger" size="sm" icon={<X className="size-4" />} loading={action.isPending} onClick={() => run('cancel')}>{t('tasks.cancel')}</Button>}
        </>
      )}>
      {task.isPending && <Skeleton className="h-64" />}
      {task.isError && <ErrorState onRetry={() => void task.refetch()} />}
      {d && (
        // Sections divided by a hairline: overview (who, which robot, when), parameters, activity.
        <div className="flex flex-col divide-y divide-slate-100 [&>*]:py-5 [&>*:first-child]:pt-0 [&>*:last-child]:pb-0">
          <section className="flex flex-col gap-3">
            <h3 className="text-card-title font-semibold text-slate-900">{t('tasks.overview')}</h3>
            <Facts rows={[
              // Priority is shown here only when the service form does not already list it among the parameters.
              ...('priority' in d.parameters ? [] : [[t('tasks.col.priority'), t(`taskForm.option.priority.${d.priority}`, { defaultValue: d.priority })] as [string, string]]),
              [t('tasks.col.createdBy'), d.requester.fullName || '—'],
              [t('tasks.col.robot'), d.robot ? <button type="button" className="font-semibold text-brand-700 hover:underline" onClick={() => { openTask(null); openRobot(d.robot!.name) }}>{d.robot.name}</button>
                : d.requestedRobot ? t('tasks.requested', { robot: d.requestedRobot }) : t('tasks.notAssigned')],
              ...(d.etaAt ? [[t('tasks.col.eta'), format.time(d.etaAt)] as [string, string]] : []),
              ...(d.previousId ? [[t('tasks.col.previous'), <button type="button" className="font-semibold text-brand-700 hover:underline" onClick={() => openTask(d.previousId)}>{taskCode(d.previousId!, d.service)}</button>] as [string, React.ReactNode]] : []),
            ]} />
            <Facts quiet rows={[
              [t('tasks.col.created'), time(d.createdAt)],
              [t('tasks.col.scheduled'), d.scheduledAt ? `${time(d.scheduledAt)}${d.repeat !== 'none' ? ` · ${t(`repeat.${d.repeat}`)}` : ''}` : t('taskForm.asap')],
              [t('tasks.col.started'), time(d.startedAt)],
              [t('tasks.col.completed'), time(d.finishedAt)],
            ]} />
            {d.error && <p role="alert" className="rounded-xl bg-red-50 px-4 py-3 text-sm text-red-700">{t(`errors.${d.error}`, { defaultValue: d.error })}</p>}
          </section>
          <Parameters task={d} />
          <section>
            <h3 className="mb-3 text-card-title font-semibold text-slate-900">{t('tasks.activity')}</h3>
            <TaskTimeline entries={d.activity} />
          </section>
        </div>
      )}
      {reassign && d && <ReassignDialog task={d} onClose={() => setReassign(false)} />}
    </Drawer>
  )
}
