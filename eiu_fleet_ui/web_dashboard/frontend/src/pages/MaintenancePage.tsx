import { CircleCheck, Play, Plus, Wrench } from 'lucide-react'
import { useState, type FormEvent } from 'react'
import { useTranslation } from 'react-i18next'
import { useFleetRobots, useMaintenance, useSaveMaintenance } from '../api/queries'
import type { MaintenanceItem, MaintenanceRow, MaintenanceStatus } from '../api/types'
import { Button } from '../components/ui/Button'
import { Card, CardHeader } from '../components/ui/Card'
import { DataTable, type Column } from '../components/ui/DataTable'
import { Dialog } from '../components/ui/Dialog'
import { Field, Input, Select, Textarea } from '../components/ui/Field'
import { FilterTabs } from '../components/ui/FilterTabs'
import { EmptyState, ErrorState, Skeleton } from '../components/ui/States'
import { useCan } from '../domain/access'
import { ServiceTag } from '../domain/services'
import { HealthBadge, toneClass } from '../domain/status'
import { PageHeader } from '../layout/PageHeader'
import { cn } from '../lib/cn'
import { useErrorText } from '../lib/errors'
import { useFormat } from '../lib/format'
import { toast } from '../lib/toast'
import { useUi } from '../lib/ui'

const STATUS_TONE: Record<MaintenanceStatus, keyof typeof toneClass> = {
  none: 'success', scheduled: 'info', due_soon: 'warning', due: 'warning', in_progress: 'warning',
}
/** Due is the stronger orange of the same warning hue; routine maintenance never turns red. */
const DUE_CLASS = 'bg-amber-100 text-amber-800 ring-amber-500/50'

function toMs(local: string): number | null {
  return local ? new Date(local).getTime() : null
}

/** Field problems of the plan form; the backend enforces the title and the window order, the rest guides the input. */
function planErrors(form: { robot: string; title: string; due: string; from: string; to: string }) {
  const errors: Partial<Record<'robot' | 'title' | 'due' | 'from' | 'to', string>> = {}
  if (!form.robot) errors.robot = 'robot'
  if (!form.title.trim()) errors.title = 'title'
  if (form.from && !form.to) errors.to = 'endMissing'
  if (form.to && !form.from) errors.from = 'startMissing'
  if (form.from && form.to && toMs(form.to)! <= toMs(form.from)!) errors.to = 'order'
  if (!form.due && !form.from && !form.to) errors.due = 'when'
  if (form.due && form.to && !errors.to && toMs(form.to)! > toMs(form.due)!) errors.due = 'dueBeforeWindow'
  return errors
}

/** Plan a maintenance item: robot (read-only when opened from a robot's row), task, due date and/or window, note. */
function PlanDialog({ robot, onClose }: { robot?: string; onClose: () => void }) {
  const { t } = useTranslation()
  const robots = useFleetRobots().data
  const save = useSaveMaintenance()
  const errorText = useErrorText()
  const [form, setForm] = useState({ robot: robot ?? '', title: '', due: '', from: '', to: '', note: '' })
  const [touched, setTouched] = useState<Partial<Record<keyof typeof form, boolean>>>({})
  const [error, setError] = useState<string | null>(null)
  const errors = planErrors(form)
  const valid = Object.keys(errors).length === 0
  // A field shows its problem once it has been left or, for the dates, changed.
  const shown = (key: keyof typeof errors) => {
    const code = errors[key]
    // "Due date or window" also shows once the task is filled in, so a disabled button is never left unexplained.
    if (!code || code === 'robot' || !(touched[key] || (code === 'when' && touched.title))) return undefined
    return t(`maintenance.error.${code}`)
  }
  const submit = (e: FormEvent) => {
    e.preventDefault()
    if (!valid) return
    save.mutate({ robot: form.robot, title: form.title.trim(), dueAt: toMs(form.due), windowStart: toMs(form.from), windowEnd: toMs(form.to), note: form.note }, {
      onSuccess: () => { toast.success(t('maintenance.planned')); onClose() },
      onError: (err) => setError(errorText(err)),
    })
  }
  const set = (key: keyof typeof form) => (e: { target: { value: string } }) => {
    const value = e.target.value
    setForm((f) => ({ ...f, [key]: value }))
    if (key === 'from' || key === 'to' || key === 'due') setTouched((x) => ({ ...x, [key]: true }))
  }
  const leave = (key: keyof typeof form) => () => setTouched((x) => ({ ...x, [key]: true }))
  return (
    <Dialog open onClose={onClose} title={t('maintenance.plan')} className="max-w-lg"
      footer={<>
        <Button variant="ghost" onClick={onClose}>{t('common.cancel')}</Button>
        <Button type="submit" form="plan-maintenance" disabled={!valid} loading={save.isPending}>{t('maintenance.plan')}</Button>
      </>}>
      <form id="plan-maintenance" onSubmit={submit} noValidate className="flex flex-col gap-4">
        <Field label={t('fleet.col.robot')} htmlFor="m-robot">
          {robot
            ? <output id="m-robot" className="flex h-12 items-center rounded-xl bg-slate-50 px-4 font-semibold text-slate-900 ring-1 ring-slate-200 ring-inset">{robot}</output>
            : (
              <Select id="m-robot" required value={form.robot} onChange={set('robot')}>
                <option value="">{t('taskForm.selectRobot')}</option>
                {(robots ?? []).map((r) => <option key={r.name} value={r.name}>{r.name}</option>)}
              </Select>
            )}
        </Field>
        <Field label={t('maintenance.itemTitle')} htmlFor="m-title" hint={t('maintenance.itemTitleHint')} error={shown('title')}>
          <Input id="m-title" required maxLength={200} value={form.title} onChange={set('title')} onBlur={leave('title')}
            aria-invalid={!!shown('title')} />
        </Field>
        <Field label={t('maintenance.dueAt')} htmlFor="m-due" error={shown('due')}>
          <Input id="m-due" type="datetime-local" value={form.due} onChange={set('due')} onBlur={leave('due')} aria-invalid={!!shown('due')} />
        </Field>
        <fieldset>
          <legend className="mb-1 text-sm font-semibold text-slate-800">{t('maintenance.window')}</legend>
          <p className="mb-3 text-sm text-slate-500">{t('maintenance.windowHint')}</p>
          <div className="grid gap-3 sm:grid-cols-2">
            <Field label={t('maintenance.windowStart')} htmlFor="m-from" error={shown('from')}>
              <Input id="m-from" type="datetime-local" value={form.from} onChange={set('from')} onBlur={leave('from')} aria-invalid={!!shown('from')} />
            </Field>
            <Field label={t('maintenance.windowEnd')} htmlFor="m-to" error={shown('to')}>
              <Input id="m-to" type="datetime-local" value={form.to} onChange={set('to')} onBlur={leave('to')} aria-invalid={!!shown('to')} />
            </Field>
          </div>
        </fieldset>
        <Field label={t('maintenance.note')} htmlFor="m-note"><Textarea id="m-note" rows={2} value={form.note} onChange={set('note')} /></Field>
        {error && <p role="alert" className="rounded-xl bg-red-50 px-4 py-3 text-sm text-red-700">{error}</p>}
      </form>
    </Dialog>
  )
}

/** Maintenance: health, maintenance status, next service and operating hours of every robot the user sees. */
export default function MaintenancePage() {
  const { t } = useTranslation()
  const format = useFormat()
  const canManage = useCan('maintenance.manage')
  const data = useMaintenance()
  const save = useSaveMaintenance()
  const errorText = useErrorText()
  const openRobot = useUi((s) => s.openRobot)
  const selectedRobot = useUi((s) => s.robot)
  const [plan, setPlan] = useState<{ robot?: string } | null>(null)
  const [filter, setFilter] = useState<'all' | 'attention'>('all')
  const act = (item: MaintenanceItem, status: MaintenanceItem['status']) => save.mutate({ id: item.id, status }, {
    onSuccess: () => toast.success(t(`maintenance.done.${status}`)),
    onError: (e) => toast.error(errorText(e)),
  })
  const rows = (data.data?.robots ?? []).filter((r) => filter === 'all' || r.status !== 'none' || r.health !== 'healthy')
  const columns: Column<MaintenanceRow>[] = [
    { key: 'robot', header: t('fleet.col.robot'), cell: (r) => <button type="button" className="font-semibold text-slate-900 hover:underline" onClick={() => openRobot(r.robot)}>{r.robot}</button> },
    { key: 'type', header: t('fleet.col.type'), cell: (r) => <ServiceTag service={r.serviceType} /> },
    { key: 'health', header: t('fleet.health'), cell: (r) => <HealthBadge health={r.health} /> },
    { key: 'status', header: t('maintenance.col.status'), cell: (r) => (
      <span className={cn('inline-flex h-6 items-center rounded-full px-2 text-xs font-semibold ring-1 ring-inset', r.status === 'due' ? DUE_CLASS : toneClass[STATUS_TONE[r.status]])}>{t(`maintenance.status.${r.status}`)}</span>
    ) },
    { key: 'next', header: t('maintenance.col.next'), cell: (r) => (r.next ? <span className="whitespace-nowrap">{format.dateTime(r.next.dueAt ?? r.next.windowStart ?? r.next.createdAt)}</span> : <span className="text-slate-500">{t('maintenance.noneRequired')}</span>) },
    { key: 'hours', header: t('maintenance.col.hours'), cell: (r) => (r.operatingHours !== null ? t('maintenance.hours', { count: r.operatingHours }) : '—') },
    { key: 'issue', header: t('maintenance.col.issue'), cell: (r) => r.issue ?? <span className="text-slate-400">—</span> },
    { key: 'actions', header: <span className="sr-only">{t('fleet.col.actions')}</span>, cell: (r) => canManage && (
      <div className="flex gap-1.5">
        {r.next?.status === 'planned' && <Button size="sm" variant="secondary" icon={<Play className="size-3.5" />} onClick={() => act(r.next!, 'in_progress')}>{t('maintenance.start')}</Button>}
        {r.next && <Button size="sm" variant="ghost" icon={<CircleCheck className="size-3.5" />} onClick={() => act(r.next!, 'done')}>{t('maintenance.markDone')}</Button>}
        {!r.next && <Button size="sm" variant="ghost" icon={<Plus className="size-3.5" />} onClick={() => setPlan({ robot: r.robot })}>{t('maintenance.plan')}</Button>}
      </div>
    ) },
  ]
  return (
    <>
      <PageHeader title={t('maintenance.title')} subtitle={t('maintenance.subtitle')}
        actions={canManage && <Button icon={<Plus className="size-4" />} onClick={() => setPlan({})}>{t('maintenance.plan')}</Button>} />
      <Card className="p-4 sm:p-5">
        <CardHeader icon={<Wrench />} title={t('maintenance.robots')} className="mb-3"
          action={<FilterTabs value={filter} onChange={setFilter} label={t('filters.status')}
            items={[{ value: 'all', label: t('filters.all') }, { value: 'attention', label: t('maintenance.needsAttention') }]} />} />
        {data.isPending && <Skeleton className="h-64" />}
        {data.isError && <ErrorState onRetry={() => void data.refetch()} />}
        {data.data && <DataTable columns={columns} rows={rows} rowKey={(r) => r.robot} minWidth={1040} selected={selectedRobot}
          empty={<EmptyState icon={<Wrench />} title={t('maintenance.empty')} />} />}
      </Card>
      {plan && <PlanDialog robot={plan.robot} onClose={() => setPlan(null)} />}
    </>
  )
}
