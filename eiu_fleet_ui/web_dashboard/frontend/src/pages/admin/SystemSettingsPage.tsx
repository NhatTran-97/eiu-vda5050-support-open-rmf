import { ClipboardList, Cog, Layers, ShieldCheck } from 'lucide-react'
import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import { useSystemSettings, useToggle } from '../../api/queries'
import { Card, CardHeader } from '../../components/ui/Card'
import { ErrorState, Skeleton } from '../../components/ui/States'
import { Switch } from '../../components/ui/Switch'
import { Tabs } from '../../components/ui/Tabs'
import { serviceIcons } from '../../domain/services'
import { AuditPanel } from '../../features/admin/AuditPanel'
import { PageHeader } from '../../layout/PageHeader'
import { useErrorText } from '../../lib/errors'
import { useLocalize } from '../../lib/i18nText'
import { toast } from '../../lib/toast'

type Tab = 'services' | 'system' | 'audit'

function Services() {
  const { t } = useTranslation()
  const localize = useLocalize()
  const settings = useSystemSettings()
  const toggle = useToggle()
  const errorText = useErrorText()
  if (settings.isPending) return <Skeleton className="h-80 rounded-2xl" />
  if (settings.isError) return <Card><ErrorState onRetry={() => void settings.refetch()} /></Card>
  const s = settings.data
  return (
    <div className="flex flex-col gap-4 lg:gap-6">
      <div className="grid gap-4 lg:grid-cols-3 lg:items-start lg:gap-6">
        {s.services.map((svc) => {
          const Icon = serviceIcons[svc.icon]
          return (
            <Card key={svc.id} className="flex flex-col gap-3 p-4 sm:px-5 sm:py-4">
              <CardHeader icon={<Icon />} title={localize(svc.name)} subtitle={localize(svc.description)} />
              <Switch label={t('settings.serviceEnabled')} hint={svc.enabled !== svc.configured ? t('settings.overridden') : undefined} checked={svc.enabled}
                disabled={toggle.isPending} onChange={(enabled) => toggle.mutate({ kind: 'service', id: svc.id, enabled }, { onError: (e) => toast.error(errorText(e)) })} />
              <dl className="grid grid-cols-[auto_1fr] gap-x-4 gap-y-1.5 text-sm">
                <dt className="text-slate-500">{t('settings.category')}</dt><dd className="font-mono text-meta text-slate-700">{svc.category}</dd>
                <dt className="text-slate-500">{t('settings.capabilities')}</dt><dd className="font-mono text-meta text-slate-700">{svc.requiredCapabilities.join(', ')}</dd>
                <dt className="text-slate-500">{t('settings.formFields')}</dt><dd className="font-mono text-meta break-words text-slate-700">{svc.taskFormSchema.map((f) => f.key).join(', ')}</dd>
              </dl>
            </Card>
          )
        })}
      </div>
      <Card className="p-4 sm:px-5 sm:py-4">
        <CardHeader icon={<Layers />} title={t('settings.fleets')} subtitle={t('settings.fleetsHint')} className="mb-3" />
        <ul className="divide-y divide-slate-100 text-sm">
          {Object.entries(s.fleets).map(([fleet, f]) => (
            <li key={fleet} className="flex flex-wrap items-baseline gap-x-6 gap-y-1 py-2"><span className="font-semibold text-slate-900">{fleet}</span>
              <span className="text-slate-600">{t('settings.primary')}: {f.service ?? '—'}</span><span className="font-mono text-meta text-slate-600">{f.capabilities.join(', ')}</span></li>
          ))}
        </ul>
        <p className="mt-3 text-xs text-slate-500">{t('settings.sources', s.sources)}</p>
      </Card>
    </div>
  )
}

function System() {
  const { t } = useTranslation()
  const settings = useSystemSettings()
  if (!settings.data) return <Skeleton className="h-80 rounded-2xl" />
  return (
    // Cards as wide as an 18rem column allows and as tall as their values; a sparse section stays small.
    <div className="grid grid-cols-[repeat(auto-fill,minmax(min(100%,18rem),1fr))] items-start gap-4 lg:gap-6">
      {Object.entries(settings.data.settings).map(([section, values]) => (
        <Card key={section} className="p-4 sm:px-5 sm:py-4">
          <CardHeader icon={<Cog />} title={section} className="mb-3" />
          <dl className="grid grid-cols-[auto_1fr] gap-x-4 gap-y-1.5 text-sm">
            {Object.entries(values).map(([k, v]) => <div key={k} className="contents"><dt className="text-slate-500">{k}</dt><dd className="font-mono text-meta break-all text-slate-700">{typeof v === 'object' ? JSON.stringify(v) : String(v)}</dd></div>)}
          </dl>
        </Card>
      ))}
      <p className="col-span-full text-body text-ops-muted">{t('settings.readOnlyHint')}</p>
    </div>
  )
}

/** Settings: robot services on or off, the fleets and their capabilities, the effective configuration, the audit log. */
export default function SystemSettingsPage() {
  const { t } = useTranslation()
  const [tab, setTab] = useState<Tab>('services')
  return (
    <>
      <PageHeader title={t('settings.title')} subtitle={t('settings.subtitle')} />
      <Tabs<Tab> value={tab} onChange={setTab} label={t('settings.title')} className="mb-4 rounded-2xl bg-surface p-1.5 shadow-card ring-1 ring-slate-900/5 lg:mb-6"
        items={[{ value: 'services', label: t('settings.tabServices'), icon: <ShieldCheck className="size-4" /> },
          { value: 'system', label: t('settings.tabSystem'), icon: <Cog className="size-4" /> },
          { value: 'audit', label: t('settings.tabAudit'), icon: <ClipboardList className="size-4" /> }]} />
      {tab === 'services' && <Services />}
      {tab === 'system' && <System />}
      {tab === 'audit' && <AuditPanel />}
    </>
  )
}
