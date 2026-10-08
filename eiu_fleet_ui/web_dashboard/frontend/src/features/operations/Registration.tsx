import { CircleAlert, CircleCheck, Radar, TriangleAlert, UserPlus } from 'lucide-react'
import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import { useRegister, useRegistration } from '../../api/queries'
import type { Finding, PendingRobot, RegistrationRequest, RegistrationResult } from '../../api/types'
import { Button } from '../../components/ui/Button'
import { Card, CardHeader } from '../../components/ui/Card'
import { Dialog } from '../../components/ui/Dialog'
import { Field, Input, Select } from '../../components/ui/Field'
import { EmptyState, ErrorState, Skeleton } from '../../components/ui/States'
import { Switch } from '../../components/ui/Switch'
import { usePermission } from '../auth/guards'
import { useErrorText } from '../../lib/errors'
import { toast } from '../../lib/toast'

/** Robots seen on the broker that no fleet has registered yet. */
export function PendingRobotsCard() {
  const { t } = useTranslation()
  const registration = useRegistration()
  const canRegister = usePermission('robots.manage')
  const [chosen, setChosen] = useState<PendingRobot | null>(null)
  const pending = registration.data?.pending ?? []

  return (
    <Card className="p-4 sm:p-5">
      <CardHeader icon={<Radar />} title={t('ops.pending.title')} className="mb-4" />
      {registration.isPending && <Skeleton className="h-24" />}
      {registration.isError && <ErrorState onRetry={() => void registration.refetch()} />}
      {registration.data && pending.length === 0 && <p className="text-sm text-slate-500">{t('ops.pending.empty')}</p>}
      {pending.length > 0 && (
        <ul className="flex flex-col gap-2">
          {pending.map((robot) => (
            <li key={`${robot.manufacturer}/${robot.serial}`} className="flex flex-wrap items-center gap-3 rounded-xl bg-slate-50 px-3.5 py-3 ring-1 ring-slate-200">
              <div className="min-w-0 flex-1">
                <p className="flex flex-wrap items-center gap-2">
                  <span className="text-card-title font-bold text-slate-900">{robot.manufacturer} / {robot.serial}</span>
                  <span className="inline-flex h-5 items-center rounded-md bg-ops-cyan-soft px-1.5 text-meta font-semibold text-ops-cyan">{t('ops.pending.discovered')}</span>
                </p>
                <p className="mt-0.5 text-meta text-slate-500">
                  {robot.series ? `${robot.series} · ` : ''}
                  {robot.pose ? (robot.pose.initialized ? `(${robot.pose.x.toFixed(1)}, ${robot.pose.y.toFixed(1)}) ${robot.pose.map}` : t('ops.pending.notLocalized')) : t('ops.pending.noState')}
                  {' · '}{t('ops.pending.seenBy', { fleets: robot.reporters.join(', ') })}
                </p>
                {robot.removed_as && <p className="text-xs text-amber-700">{t('ops.pending.removedAs', robot.removed_as)}</p>}
              </div>
              {canRegister && (
                <Button size="sm" icon={<UserPlus className="size-4" />} onClick={() => setChosen(robot)}>{t('ops.pending.register')}</Button>
              )}
            </li>
          ))}
        </ul>
      )}
      {chosen && <RegisterDialog robot={chosen} onClose={() => setChosen(null)} />}
    </Card>
  )
}

function Findings({ title, items, tone }: { title: string; items: Finding[]; tone: 'error' | 'warning' }) {
  if (items.length === 0) return null
  const Icon = tone === 'error' ? CircleAlert : TriangleAlert
  return (
    <div className={tone === 'error' ? 'text-red-700' : 'text-amber-700'}>
      <p className="text-sm font-semibold">{title}</p>
      <ul className="mt-1 flex flex-col gap-1 text-sm">
        {items.map((f, i) => (
          <li key={`${f.code}-${i}`} className="flex gap-2"><Icon className="mt-0.5 size-4 shrink-0" />{f.message || f.code}</li>
        ))}
      </ul>
    </div>
  )
}

function RegisterDialog({ robot, onClose }: { robot: PendingRobot; onClose: () => void }) {
  const { t } = useTranslation()
  const fleets = useRegistration().data?.fleets ?? []
  const register = useRegister()
  const errorText = useErrorText()
  const [fleet, setFleet] = useState(robot.suggestion.fleet || fleets[0]?.fleet || '')
  const [name, setName] = useState(robot.suggestion.name)
  const [charger, setCharger] = useState(robot.suggestion.charger)
  const [responsiveWait, setResponsiveWait] = useState(false)
  const [confirmUnverified, setConfirmUnverified] = useState(false)
  const [checked, setChecked] = useState<RegistrationResult | null>(null)
  const chargers = fleets.find((f) => f.fleet === fleet)?.chargers ?? []

  const request = (action: RegistrationRequest['action']): RegistrationRequest => ({
    action, fleet, name: name.trim(), manufacturer: robot.manufacturer, serial: String(robot.serial), charger,
    responsiveWait, confirmUnverified,
  })
  const edit = <T,>(set: (v: T) => void) => (v: T) => {
    set(v)
    setChecked(null)
  }
  const complete = !!fleet && !!name.trim() && !!charger
  const canAdd = !!checked && (checked.ok || (checked.needsConfirmation && confirmUnverified))

  const check = () => register.mutate(request('check'), { onSuccess: setChecked, onError: (e) => toast.error(errorText(e)) })
  const add = () => register.mutate(request('add'), {
    onSuccess: (result) => {
      if (result.ok) {
        const note = result.persisted ? '' : ` (${t('ops.register.notSaved')})`
        toast.success(t('ops.register.registered', { name: name.trim(), fleet }) + note)
        onClose()
      } else {
        setChecked(result)
      }
    },
    onError: (e) => toast.error(errorText(e)),
  })

  return (
    <Dialog
      open
      onClose={onClose}
      className="max-w-lg"
      title={t('ops.register.title', { id: `${robot.manufacturer}/${robot.serial}` })}
      footer={<>
        <Button variant="secondary" onClick={onClose}>{t('common.cancel')}</Button>
        <Button variant="secondary" disabled={!complete} loading={register.isPending && register.variables?.action === 'check'} onClick={check}>{t('ops.register.check')}</Button>
        <Button disabled={!complete || !canAdd} loading={register.isPending && register.variables?.action === 'add'} onClick={add}>{t('ops.register.submit')}</Button>
      </>}
    >
      <div className="flex flex-col gap-4">
        <Field label={t('ops.register.fleet')} htmlFor="reg-fleet">
          <Select id="reg-fleet" value={fleet} onChange={(e) => edit(setFleet)(e.target.value)}>
            {fleets.map((f) => <option key={f.fleet} value={f.fleet}>{f.fleet}{f.series ? ` (${f.series})` : ''}</option>)}
          </Select>
        </Field>
        <Field label={t('ops.register.name')} htmlFor="reg-name">
          <Input id="reg-name" value={name} autoComplete="off" onChange={(e) => edit(setName)(e.target.value)} />
        </Field>
        <Field label={t('ops.register.charger')} htmlFor="reg-charger">
          <Select id="reg-charger" value={charger} onChange={(e) => edit(setCharger)(e.target.value)}>
            <option value="" disabled>—</option>
            {chargers.map((c) => (
              <option key={c.name} value={c.name} disabled={!!c.used_by && !c.used_by_removed}>
                {c.name}{c.used_by ? ` · ${c.used_by}` : ''}
              </option>
            ))}
          </Select>
        </Field>
        <Switch label={t('ops.register.responsiveWait')} checked={responsiveWait} onChange={edit(setResponsiveWait)} />
        {checked && (
          <div className="flex flex-col gap-3 rounded-xl bg-slate-50 p-3">
            <p className={checked.ok ? 'flex items-center gap-2 text-sm font-semibold text-emerald-700' : 'flex items-center gap-2 text-sm font-semibold text-red-700'}>
              {checked.ok ? <CircleCheck className="size-4" /> : <CircleAlert className="size-4" />}
              {checked.ok ? t('ops.register.passed') : checked.needsConfirmation ? t('ops.register.needsConfirmation') : t('ops.register.failed')}
            </p>
            <Findings title={t('ops.register.errors')} items={checked.errors} tone="error" />
            <Findings title={t('ops.register.warnings')} items={checked.warnings} tone="warning" />
            {checked.needsConfirmation && (
              <label className="flex items-center gap-2 text-sm font-medium text-slate-800">
                <input type="checkbox" className="size-4 accent-brand-600" checked={confirmUnverified} onChange={(e) => setConfirmUnverified(e.target.checked)} />
                {t('ops.register.confirmUnverified')}
              </label>
            )}
          </div>
        )}
      </div>
    </Dialog>
  )
}

export function NoRobots() {
  const { t } = useTranslation()
  return <EmptyState icon={<Radar />} title={t('ops.robots.empty')} />
}
