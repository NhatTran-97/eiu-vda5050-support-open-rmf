import { Bell, Database, KeyRound, Languages, ShieldCheck, SunMoon, UserRound } from 'lucide-react'
import { useState, type FormEvent } from 'react'
import { useTranslation } from 'react-i18next'
import { useCatalog, useChangePassword, useConfig, useMe, useResetDemo, useUpdatePreferences } from '../api/queries'
import { useServiceName } from '../domain/services'
import { useLocalize } from '../lib/i18nText'
import type { NotificationPrefs } from '../api/types'
import { Button } from '../components/ui/Button'
import { Card, CardHeader } from '../components/ui/Card'
import { Field, Input } from '../components/ui/Field'
import { Switch } from '../components/ui/Switch'
import { LanguageSwitch } from '../features/auth/LanguageSwitch'
import { ThemeSwitch } from '../features/auth/ThemeSwitch'
import { PageHeader } from '../layout/PageHeader'
import { useErrorText } from '../lib/errors'
import { initials } from '../lib/text'
import { toast } from '../lib/toast'

function PasswordForm() {
  const { t } = useTranslation()
  const config = useConfig().data
  const change = useChangePassword()
  const errorText = useErrorText()
  const [current, setCurrent] = useState('')
  const [next, setNext] = useState('')
  const [confirm, setConfirm] = useState('')
  const [error, setError] = useState<string | null>(null)
  const min = config?.minPasswordLength ?? 8

  const submit = (e: FormEvent) => {
    e.preventDefault()
    if (next !== confirm) {
      setError(t('settings.passwordMismatch'))
      return
    }
    change.mutate({ currentPassword: current, newPassword: next }, {
      onSuccess: () => {
        toast.success(t('settings.passwordChanged'))
        setCurrent('')
        setNext('')
        setConfirm('')
        setError(null)
      },
      onError: (err) => setError(errorText(err)),
    })
  }

  return (
    <form onSubmit={submit} className="mt-4 grid gap-4">
      <Field label={t('settings.currentPassword')} htmlFor="current-password">
        <Input id="current-password" type="password" autoComplete="current-password" required value={current} onChange={(e) => setCurrent(e.target.value)} />
      </Field>
      <Field label={t('settings.newPassword')} htmlFor="new-password" hint={t('settings.passwordHint', { count: min })}>
        <Input id="new-password" type="password" autoComplete="new-password" required minLength={min} value={next} onChange={(e) => setNext(e.target.value)} />
      </Field>
      <Field label={t('settings.confirmPassword')} htmlFor="confirm-password">
        <Input id="confirm-password" type="password" autoComplete="new-password" required value={confirm} onChange={(e) => setConfirm(e.target.value)} />
      </Field>
      {error && <p role="alert" className="text-sm font-medium text-red-600">{error}</p>}
      <Button type="submit" loading={change.isPending} className="justify-self-start">{t('settings.changePassword')}</Button>
    </form>
  )
}

/** The services, zones and permissions an admin granted to this account. */
function AccessCard() {
  const { t } = useTranslation()
  const me = useMe().data
  const serviceName = useServiceName()
  const localize = useLocalize()
  const zones = useCatalog().data?.zones
  if (!me) return null
  const chip = 'rounded-full bg-slate-100 px-2.5 py-1 text-xs font-semibold text-slate-700'
  return (
    <Card className="p-4 sm:p-6">
      <CardHeader icon={<KeyRound />} title={t('account.access')} subtitle={t('account.accessHint')} />
      <dl className="mt-5 space-y-4 text-sm">
        <div><dt className="mb-1.5 text-slate-500">{t('users.col.services')}</dt>
          <dd className="flex flex-wrap gap-1.5">{me.allowedServices.length ? me.allowedServices.map((s) => <span key={s} className={chip}>{serviceName(s)}</span>) : t('users.none')}</dd></div>
        <div><dt className="mb-1.5 text-slate-500">{t('users.col.zones')}</dt>
          <dd className="flex flex-wrap gap-1.5">{me.allZones ? <span className={chip}>{t('users.allZones')}</span>
            : me.allowedZones.map((z) => <span key={z} className={chip}>{localize(zones?.find((x) => x.id === z)?.name ?? { vi: z, en: z })}</span>)}</dd></div>
        <div><dt className="mb-1.5 text-slate-500">{t('users.permissions')}</dt>
          <dd className="flex flex-wrap gap-1.5">{me.permissions.map((p) => <span key={p} className={chip}>{t(`permission.${p}`)}</span>)}</dd></div>
      </dl>
    </Card>
  )
}

export default function AccountPage() {
  const { t } = useTranslation()
  const me = useMe().data
  const config = useConfig().data
  const update = useUpdatePreferences()
  const reset = useResetDemo()
  if (!me) return null

  const setPref = (patch: Partial<NotificationPrefs>) =>
    update.mutate({ notificationPrefs: { ...me.notificationPrefs, ...patch } }, { onSuccess: () => toast.success(t('settings.saved')) })

  return (
    <>
      <PageHeader title={t('account.title')} subtitle={t('account.subtitle')} />
      <div className="grid grid-cols-1 gap-4 lg:gap-6 xl:grid-cols-2">
        <Card className="p-4 sm:p-6">
          <CardHeader icon={<UserRound />} title={t('settings.profile')} />
          <div className="mt-5 flex items-center gap-4">
            <span className="flex size-16 shrink-0 items-center justify-center rounded-full bg-navy-800 text-xl font-bold text-white">{initials(me.fullName)}</span>
            <dl className="min-w-0 space-y-1 text-sm">
              <div><dt className="sr-only">{t('settings.name')}</dt><dd className="truncate text-lg font-bold text-slate-900">{me.fullName}</dd></div>
              <div><dt className="sr-only">{t('settings.email')}</dt><dd className="truncate text-slate-600">{me.email}</dd></div>
              <div className="flex items-center gap-2"><dt className="text-slate-500">{t('settings.role')}:</dt><dd><span className="rounded-full bg-brand-50 px-2 py-0.5 text-xs font-semibold text-brand-700">{t(`role.${me.role}`)}</span></dd></div>
            </dl>
          </div>
        </Card>

        <AccessCard />

        <Card className="p-4 sm:p-6">
          <CardHeader icon={<Languages />} title={t('settings.language')} subtitle={t('settings.languageHint')} />
          <div className="mt-5 flex items-center gap-4">
            <LanguageSwitch />
            <span className="text-sm text-slate-600">{me.locale === 'vi' ? 'Tiếng Việt' : 'English'}</span>
          </div>
        </Card>

        <Card className="p-4 sm:p-6">
          <CardHeader icon={<SunMoon />} title={t('theme.label')} subtitle={t('theme.hint')} />
          <ThemeSwitch labels className="mt-5" />
        </Card>

        <Card className="p-4 sm:p-6">
          <CardHeader icon={<Bell />} title={t('settings.notifications')} />
          <div className="mt-5 space-y-5">
            <Switch label={t('settings.deliveryUpdates')} hint={t('settings.deliveryUpdatesHint')} checked={me.notificationPrefs.deliveryUpdates} disabled={update.isPending} onChange={(v) => setPref({ deliveryUpdates: v })} />
            <Switch label={t('settings.delays')} hint={t('settings.delaysHint')} checked={me.notificationPrefs.delays} disabled={update.isPending} onChange={(v) => setPref({ delays: v })} />
          </div>
        </Card>

        <Card className="p-4 sm:p-6">
          <CardHeader icon={<ShieldCheck />} title={t('settings.security')} />
          <PasswordForm />
        </Card>

        {config?.demo && (
          <Card className="p-4 sm:p-6 xl:col-span-2">
            <CardHeader icon={<Database />} title={t('settings.demo')} subtitle={t('settings.demoHint')} />
            <Button variant="danger" className="mt-4" loading={reset.isPending} onClick={() => reset.mutate(undefined, { onSuccess: () => toast.success(t('settings.resetDone')) })}>
              {t('settings.reset')}
            </Button>
          </Card>
        )}
      </div>
    </>
  )
}
