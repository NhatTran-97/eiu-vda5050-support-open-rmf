import { ChevronDown, Send, X } from 'lucide-react'
import { useMemo, useState, type FormEvent } from 'react'
import { useTranslation } from 'react-i18next'
import { useCatalog, useCreateTask, useFleetRobots, useLocations, useTemplates } from '../../api/queries'
import type { FormField, Repeat, ServiceDef, TaskDetail } from '../../api/types'
import { Button } from '../../components/ui/Button'
import { Field, Input, Select, Textarea } from '../../components/ui/Field'
import { useCan, useZoneAllowed } from '../../domain/access'
import { cn } from '../../lib/cn'
import { useErrorText } from '../../lib/errors'
import { useLocalize } from '../../lib/i18nText'

const SCHEDULE_LEAD_MS = 5 * 60_000
const REPEATS: Repeat[] = ['none', 'daily', 'weekdays', 'weekly']

type Value = string | number | string[]

/** Value for <input type="datetime-local"> in the browser's time zone. */
function toLocalInput(ts: number): string {
  const d = new Date(ts)
  const pad = (n: number) => String(n).padStart(2, '0')
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}T${pad(d.getHours())}:${pad(d.getMinutes())}`
}

function initialValues(service: ServiceDef): Record<string, Value> {
  const out: Record<string, Value> = {}
  for (const f of service.taskFormSchema) {
    if (f.default !== undefined) out[f.key] = f.default
    else if (f.type === 'locations') out[f.key] = []
  }
  return out
}

function empty(value: Value | undefined): boolean {
  return value === undefined || value === '' || (Array.isArray(value) && value.length === 0)
}

/** Label of a form field: the service's own label, else the shared translation of its key. */
export function useFieldLabel() {
  const { t } = useTranslation()
  const localize = useLocalize()
  return (f: FormField) => (f.label ? localize(f.label) : t(`taskForm.field.${f.key}`, { defaultValue: f.key }))
}

/**
 * Task form built from the service's schema (`taskFormSchema`): one control per field type, the shared schedule
 * control, templates of the service and, with `fleet.assign`, a manual robot choice.
 */
export function DynamicTaskForm({ service, onCreated, onCancel }: {
  service: ServiceDef
  onCreated: (task: TaskDetail) => void
  onCancel?: () => void
}) {
  const { t } = useTranslation()
  const localize = useLocalize()
  const errorText = useErrorText()
  const fieldLabel = useFieldLabel()
  const zoneAllowed = useZoneAllowed()
  const canSchedule = useCan('task.schedule')
  const canAssign = useCan('fleet.assign')
  const locations = useLocations().data
  const catalog = useCatalog().data
  const templates = useTemplates().data?.filter((tpl) => tpl.service === service.id && tpl.available)
  const robots = useFleetRobots(canAssign).data
  const create = useCreateTask()
  const [values, setValues] = useState<Record<string, Value>>(() => initialValues(service))
  const [when, setWhen] = useState<'now' | 'later'>('now')
  const [scheduledLocal, setScheduledLocal] = useState('')
  const [repeat, setRepeat] = useState<Repeat>('none')
  const [robot, setRobot] = useState('')
  const [advanced, setAdvanced] = useState(false)
  const [error, setError] = useState<{ text: string; field?: string } | null>(null)
  const schedule = service.taskFormSchema.find((f) => f.type === 'schedule')

  const places = useMemo(() => [...(locations ?? [])].filter((l) => zoneAllowed(l.zone))
    .sort((a, b) => localize(a.name).localeCompare(localize(b.name))), [locations, localize, zoneAllowed])
  const capable = (robots ?? []).filter((r) => r.services.includes(service.id) && r.connection === 'online')

  const set = (key: string, value: Value) => {
    setError(null)
    setValues((v) => ({ ...v, [key]: value }))
  }

  const problems = new Map<string, string>()
  for (const f of service.taskFormSchema) {
    if (f.type === 'schedule') continue
    if (f.required && empty(values[f.key])) problems.set(f.key, t('taskForm.required'))
    if (f.differsFrom && !empty(values[f.key]) && values[f.key] === values[f.differsFrom]) problems.set(f.key, t('taskForm.mustDiffer', { other: fieldLabel(service.taskFormSchema.find((x) => x.key === f.differsFrom)!) }))
  }
  const blocked = problems.size > 0 || (when === 'later' && !scheduledLocal)

  const submit = (e: FormEvent) => {
    e.preventDefault()
    if (blocked) return
    const parameters = Object.fromEntries(Object.entries(values).filter(([, v]) => !empty(v)))
    const scheduledAt = when === 'later' ? new Date(scheduledLocal).getTime() : null
    create.mutate(
      { serviceType: service.id, parameters, scheduledAt, repeat: scheduledAt ? repeat : 'none', robot: robot || undefined },
      {
        onSuccess: onCreated,
        onError: (err) => setError({ text: errorText(err), field: (err as { message?: string }).message }),
      },
    )
  }

  const control = (f: FormField) => {
    const id = `task-${f.key}`
    const value = values[f.key]
    const parent = f.filter ? String(values[f.filter] ?? '') : ''
    switch (f.type) {
      case 'location':
        return (
          <Select id={id} value={String(value ?? '')} onChange={(e) => set(f.key, e.target.value)}>
            <option value="">{t('taskForm.selectLocation')}</option>
            {places.map((l) => <option key={l.id} value={l.id}>{localize(l.name)} · {localize(l.building)}</option>)}
          </Select>
        )
      case 'locations': {
        const list = (value as string[] | undefined) ?? []
        const name = (lid: string) => localize(places.find((l) => l.id === lid)?.name ?? { vi: lid, en: lid })
        return (
          <div>
            {list.length > 0 && (
              <ol className="mb-2 flex flex-wrap gap-2">
                {list.map((lid, i) => (
                  <li key={`${lid}-${i}`} className="flex h-8 items-center gap-1.5 rounded-lg bg-brand-50 pr-1 pl-2.5 text-sm font-semibold text-brand-800 ring-1 ring-brand-200">
                    <span className="text-xs text-brand-500">{i + 1}.</span>{name(lid)}
                    <button type="button" aria-label={t('taskForm.removeStop', { name: name(lid) })} onClick={() => set(f.key, list.filter((_, j) => j !== i))}
                      className="flex size-6 items-center justify-center rounded-md text-brand-500 hover:bg-brand-100"><X className="size-3.5" /></button>
                  </li>
                ))}
              </ol>
            )}
            <Select id={id} value="" disabled={list.length >= (f.maxItems ?? 8)} onChange={(e) => e.target.value && set(f.key, [...list, e.target.value])}>
              <option value="">{t('taskForm.addStop')}</option>
              {places.map((l) => <option key={l.id} value={l.id} disabled={list[list.length - 1] === l.id}>{localize(l.name)}</option>)}
            </Select>
          </div>
        )
      }
      case 'zone':
        return (
          <Select id={id} value={String(value ?? '')} onChange={(e) => set(f.key, e.target.value)}>
            <option value="">{t('taskForm.selectZone')}</option>
            {(catalog?.zones ?? []).filter((z) => z.allowed && z.enabled).map((z) => <option key={z.id} value={z.id}>{localize(z.name)}</option>)}
          </Select>
        )
      case 'area':
      case 'route': {
        const items = (f.type === 'area' ? catalog?.areas : catalog?.routes) ?? []
        const shown = items.filter((a) => zoneAllowed(a.zone) && (!parent || a.zone === parent))
        return (
          <Select id={id} value={String(value ?? '')} disabled={!!f.filter && !parent} onChange={(e) => set(f.key, e.target.value)}>
            <option value="">{t(f.filter && !parent ? 'taskForm.pickZoneFirst' : f.type === 'area' ? 'taskForm.selectArea' : 'taskForm.selectRoute')}</option>
            {shown.map((a) => <option key={a.id} value={a.id}>{localize(a.name)}</option>)}
          </Select>
        )
      }
      case 'select':
        return (
          <div role="radiogroup" aria-labelledby={`${id}-label`} className="flex flex-wrap gap-2">
            {(f.options ?? []).map((o) => (
              <button key={o} type="button" role="radio" aria-checked={value === o} onClick={() => set(f.key, o)}
                className={cn('h-10 rounded-xl border px-3.5 text-sm font-medium transition-colors',
                  value === o ? 'border-brand-500 bg-brand-50 text-brand-700' : 'border-slate-200 text-slate-600 hover:border-slate-300')}>
                {t(`taskForm.option.${f.key}.${o}`, { defaultValue: o })}
              </button>
            ))}
          </div>
        )
      case 'number':
        return (
          <div className="flex items-center gap-2">
            <Input id={id} type="number" min={f.min} max={f.max} value={value === undefined ? '' : String(value)} className="w-32"
              onChange={(e) => set(f.key, e.target.value === '' ? '' : Number(e.target.value))} />
            {f.unit && <span className="text-sm text-slate-500">{t(`taskForm.unit.${f.unit}`, { defaultValue: f.unit })}</span>}
          </div>
        )
      case 'text':
        return <Textarea id={id} rows={2} maxLength={f.max} placeholder={t('taskForm.notePlaceholder')} value={String(value ?? '')} onChange={(e) => set(f.key, e.target.value)} />
      default:
        return null
    }
  }

  return (
    <form onSubmit={submit} className="flex flex-col gap-5">
      {templates && templates.length > 0 && (
        <div>
          <p className="mb-2 text-xs font-semibold tracking-wide text-slate-500 uppercase">{t('taskForm.templates')}</p>
          <div className="flex flex-wrap gap-2">
            {templates.map((tpl) => (
              <button key={tpl.id} type="button" className="rounded-xl bg-slate-50 px-3 py-2 text-left text-sm ring-1 ring-slate-200 hover:bg-brand-50 hover:ring-brand-200"
                onClick={() => setValues((v) => ({
                  ...v,
                  ...(tpl.pickupId ? { pickup: tpl.pickupId } : {}),
                  ...(tpl.dropoffId ? { dropoff: tpl.dropoffId } : {}),
                  ...(tpl.kind === 'delivery' ? { itemType: tpl.packageType } : {}),
                  ...(tpl.routeId ? { route: tpl.routeId, zone: catalog?.routes.find((r) => r.id === tpl.routeId)?.zone ?? '', rounds: tpl.rounds } : {}),
                }))}>
                <span className="block font-semibold text-slate-900">{localize(tpl.name)}</span>
                <span className="block text-xs text-slate-500">{localize(tpl.subtitle)}</span>
              </button>
            ))}
          </div>
        </div>
      )}

      {service.taskFormSchema.filter((f) => f.type !== 'schedule').map((f) => (
        <Field key={f.key} htmlFor={`task-${f.key}`}
          label={<span id={`task-${f.key}-label`}>{fieldLabel(f)}{!f.required && <span className="ml-1 font-normal text-slate-400">{t('common.optional')}</span>}</span>}
          error={error?.field === f.key ? error.text : undefined}
          hint={f.type === 'text' ? `${String(values[f.key] ?? '').length}/${f.max}` : undefined}>
          {control(f)}
        </Field>
      ))}

      {schedule && (
        <fieldset>
          <legend className="mb-1.5 text-sm font-semibold text-slate-800">{t('taskForm.when')}</legend>
          <div className="flex flex-wrap gap-2">
            {(['now', 'later'] as const).map((w) => (
              <label key={w} className={cn('flex h-10 items-center gap-2 rounded-xl border px-3.5 text-sm font-medium',
                !canSchedule && w === 'later' ? 'cursor-not-allowed opacity-50' : 'cursor-pointer',
                when === w ? 'border-brand-500 bg-brand-50 text-brand-700' : 'border-slate-200 text-slate-600')}>
                <input type="radio" name="when" className="accent-brand-600" checked={when === w} disabled={!canSchedule && w === 'later'}
                  onChange={() => {
                    setWhen(w)
                    if (w === 'later' && !scheduledLocal) setScheduledLocal(toLocalInput(Date.now() + 60 * 60_000))
                  }} />
                {t(w === 'now' ? 'taskForm.asap' : 'taskForm.schedule')}
              </label>
            ))}
            {when === 'later' && (
              <Input type="datetime-local" aria-label={t('taskForm.scheduleAt')} required min={toLocalInput(Date.now() + SCHEDULE_LEAD_MS)}
                value={scheduledLocal} onChange={(e) => setScheduledLocal(e.target.value)} className="h-10 w-auto min-w-56 flex-1" />
            )}
          </div>
          {when === 'later' && schedule.repeat && (
            <div className="mt-3 flex items-center gap-3">
              <label htmlFor="task-repeat" className="text-sm text-slate-600">{t('taskForm.repeat')}</label>
              <Select id="task-repeat" value={repeat} onChange={(e) => setRepeat(e.target.value as Repeat)} className="h-10 w-48">
                {REPEATS.map((r) => <option key={r} value={r}>{t(`repeat.${r}`)}</option>)}
              </Select>
            </div>
          )}
        </fieldset>
      )}

      {canAssign && (
        <div className="rounded-xl ring-1 ring-slate-200">
          <button type="button" aria-expanded={advanced} onClick={() => setAdvanced((v) => !v)} className="flex w-full items-center gap-2 px-4 py-3 text-sm font-semibold text-slate-700">
            <span className="flex-1 text-left">{t('taskForm.advanced')}</span>
            <ChevronDown className={cn('size-4 text-slate-400 transition-transform', advanced && 'rotate-180')} />
          </button>
          {advanced && (
            <div className="px-4 pb-4">
              <Field label={t('taskForm.robot')} htmlFor="task-robot" hint={t('taskForm.robotHint')}>
                <Select id="task-robot" value={robot} onChange={(e) => setRobot(e.target.value)}>
                  <option value="">{t('taskForm.autoAssign')}</option>
                  {capable.map((r) => <option key={r.name} value={r.name}>{r.name} · {t(`robotStatus.${r.status}`)} · {Math.round(r.battery)}%</option>)}
                </Select>
              </Field>
            </div>
          )}
        </div>
      )}

      {error && !error.field && <p role="alert" className="rounded-xl bg-red-50 px-4 py-3 text-sm font-medium text-red-700">{error.text}</p>}
      {!service.available && <p className="rounded-xl bg-amber-50 px-4 py-3 text-sm text-amber-800">{t('taskForm.noFleet')}</p>}

      <div className="flex flex-col-reverse gap-2 sm:flex-row sm:justify-end">
        {onCancel && <Button variant="ghost" onClick={onCancel}>{t('common.cancel')}</Button>}
        <Button type="submit" loading={create.isPending} disabled={blocked} icon={<Send className="size-4" />}>
          {t('taskForm.submit', { service: localize(service.name) })}
        </Button>
      </div>
    </form>
  )
}
