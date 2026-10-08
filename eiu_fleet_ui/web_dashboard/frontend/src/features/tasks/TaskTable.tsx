import { useTranslation } from 'react-i18next'
import type { FleetTask } from '../../api/types'
import { DataTable, type Column } from '../../components/ui/DataTable'
import { ServiceTag } from '../../domain/services'
import { TaskStatusBadge } from '../../domain/status'
import { useFormat } from '../../lib/format'
import { useLocalize } from '../../lib/i18nText'
import { useUi } from '../../lib/ui'
import { fromDelivery, taskCode, useTaskPlace } from './taskText'

/** Tasks as a table; a row opens the task drawer. */
export function TaskTable({ tasks, empty }: { tasks: FleetTask[]; empty: React.ReactNode }) {
  const { t } = useTranslation()
  const format = useFormat()
  const localize = useLocalize()
  const { place, text } = useTaskPlace()
  const openTask = useUi((s) => s.openTask)
  const selected = useUi((s) => s.task)
  const origin = (d: FleetTask) => (d.kind === 'clean' ? (d.area ? localize(d.area.name) : '—') : d.kind === 'patrol' ? text(fromDelivery(d)) : place(d.pickup.id))
  const columns: Column<FleetTask>[] = [
    { key: 'id', header: t('tasks.col.id'), cell: (d) => <span className="font-bold whitespace-nowrap text-slate-900">{taskCode(d.id, d.service)}</span> },
    { key: 'service', header: t('tasks.col.service'), cell: (d) => <ServiceTag service={d.service} /> },
    { key: 'robot', header: t('tasks.col.robot'), cell: (d) => d.robot?.name ?? <span className="text-slate-400">—</span> },
    { key: 'requester', header: t('tasks.col.requester'), cell: (d) => <span className="whitespace-nowrap">{d.requester.fullName || '—'}</span> },
    { key: 'origin', header: t('tasks.col.origin'), cell: (d) => <span className="block max-w-56 truncate">{origin(d)}</span> },
    { key: 'destination', header: t('tasks.col.destination'), cell: (d) => (d.kind === 'delivery' ? place(d.dropoff.id) : <span className="text-slate-400">—</span>) },
    { key: 'status', header: t('tasks.col.status'), cell: (d) => <TaskStatusBadge state={d.state} /> },
    { key: 'priority', header: t('tasks.col.priority'), cell: (d) => (d.priority === 'high' ? <span className="font-semibold text-amber-700">{t('taskForm.option.priority.high')}</span> : <span className="text-slate-500">{t('taskForm.option.priority.normal')}</span>) },
    { key: 'created', header: t('tasks.col.created'), cell: (d) => <span className="whitespace-nowrap text-slate-500">{format.dateTime(d.createdAt)}</span> },
    { key: 'scheduled', header: t('tasks.col.scheduled'), cell: (d) => <span className="whitespace-nowrap text-slate-500">{d.scheduledAt ? format.dateTime(d.scheduledAt) : '—'}</span> },
  ]
  return <DataTable columns={columns} rows={tasks} rowKey={(d) => d.id} onRow={(d) => openTask(d.id)} selected={selected} empty={empty} minWidth={1080} />
}
