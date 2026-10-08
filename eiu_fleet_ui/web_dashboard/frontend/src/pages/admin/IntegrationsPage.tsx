import { Activity, Boxes, Cable, Network, Plug, RadioTower, Server } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { useIntegrations } from '../../api/queries'
import type { IntegrationCard, IntegrationId } from '../../api/types'
import { Card, CardHeader } from '../../components/ui/Card'
import { Facts } from '../../components/ui/Drawer'
import { EmptyState, ErrorState, Skeleton } from '../../components/ui/States'
import { healthDot } from '../../features/admin/Health'
import { AdapterCard } from '../../features/operations/AdapterCard'
import { PageHeader } from '../../layout/PageHeader'
import { cn } from '../../lib/cn'
import { useFormat } from '../../lib/format'

const ICONS: Record<IntegrationId, typeof Plug> = { open_rmf: Network, ros2_gateway: Server, vda5050: Boxes, mqtt: RadioTower, vendor: Cable }

function value(v: unknown, yes: string, no: string): string {
  if (v === null || v === undefined || v === '') return '—'
  if (typeof v === 'boolean') return v ? yes : no
  if (typeof v === 'object') return JSON.stringify(v)
  return String(v)
}

/** A configuration value: an object becomes one "key value" line per field, a technical identifier stays monospace. */
function ConfigValue({ v }: { v: unknown }) {
  const { t } = useTranslation()
  const yes = t('admin.yes')
  const no = t('admin.no')
  if (v && typeof v === 'object' && !Array.isArray(v)) {
    return (
      <span className="flex flex-col gap-0.5">
        {Object.entries(v as Record<string, unknown>).map(([k, x]) => (
          <span key={k} className="flex flex-wrap gap-x-2"><span className="font-mono text-meta text-slate-500">{k}</span><span className="font-mono text-meta text-slate-700">{value(x, yes, no)}</span></span>
        ))}
      </span>
    )
  }
  return <span className="font-mono text-meta break-all text-slate-700">{value(v, yes, no)}</span>
}

function IntegrationTile({ card }: { card: IntegrationCard }) {
  const { t, i18n } = useTranslation()
  // A log line in words: the alert title when the code has one, the adapter's own title for adapter.attention, else the code.
  const logText = (l: IntegrationCard['logs'][number]) => {
    const params = (l.params ?? {}) as Record<string, string>
    if (l.code === 'adapter.attention' && params.title) return params.title
    return i18n.exists(`alerts.code.${l.code}.title`) ? t(`alerts.code.${l.code}.title`, params) : l.code
  }
  const format = useFormat()
  const Icon = ICONS[card.id]
  const status = card.status === 'not_configured' ? 'unknown' : card.status
  return (
    <Card className="flex flex-col gap-3 p-4 sm:px-5 sm:py-4">
      <CardHeader icon={<Icon />} title={t(`integrations.name.${card.id}`)}
        action={<span className="flex items-center gap-2 text-sm font-semibold text-slate-700"><span className={cn('size-2.5 rounded-full', healthDot[status])} />{t(`integrations.status.${card.status}`)}</span>} />
      <Facts rows={[
        [t('integrations.lastUpdate'), card.lastUpdateAt ? format.dateTime(card.lastUpdateAt) : '—'],
        [t('integrations.fleets'), card.fleets.length ? card.fleets.join(', ') : '—'],
        ...Object.entries(card.config).map(([k, v]) => [t(`integrations.config.${k}`, { defaultValue: k }), <ConfigValue v={v} />] as [string, React.ReactNode]),
      ]} />
      <section className="border-t border-slate-100 pt-3">
        <h3 className="mb-1.5 text-xs font-semibold tracking-wide text-slate-500 uppercase">{t('integrations.logs')}</h3>
        {card.logs.length === 0 ? <p className="text-meta text-slate-500">{t('integrations.noLogs')}</p> : (
          <ul className="max-h-48 divide-y divide-slate-100 overflow-y-auto text-sm">
            {card.logs.map((l, i) => (
              <li key={i} className="flex gap-3 py-1.5">
                <span className="shrink-0 text-meta whitespace-nowrap text-slate-500 tabular-nums">{format.dateTime(l.at)}</span>
                <span className="min-w-0 text-slate-800">{logText(l)}</span>
              </li>
            ))}
          </ul>
        )}
      </section>
    </Card>
  )
}

/** Integrations: Open-RMF, the ROS 2 gateway, VDA5050 fleet adapters, MQTT and vendor APIs; status, fleets, configuration, logs. */
export default function IntegrationsPage() {
  const { t } = useTranslation()
  const cards = useIntegrations()
  const adapters = cards.data?.find((c) => c.id === 'vda5050')?.adapters ?? []
  return (
    <>
      <PageHeader title={t('integrations.title')} subtitle={t('integrations.subtitle')} />
      {cards.isPending && <Skeleton className="h-96 rounded-2xl" />}
      {cards.isError && <Card><ErrorState onRetry={() => void cards.refetch()} /></Card>}
      {cards.data && (
        <div className="flex flex-col gap-4 lg:gap-6">
          {/* Cards keep their own height: a sparse one (e.g. the vendor API) is not stretched to its neighbour. */}
          <div className="grid gap-4 lg:grid-cols-2 lg:items-start 2xl:grid-cols-3 lg:gap-6">{cards.data.map((c) => <IntegrationTile key={c.id} card={c} />)}</div>
          {/* The adapter section is as wide as its cards (22rem each, wrapping), so one adapter does not leave a wide empty band. */}
          <Card className="p-4 sm:px-5 sm:py-4 lg:w-fit lg:max-w-full">
            <CardHeader icon={<Activity />} title={t('ops.system.adapters')} className="mb-3" />
            {adapters.length === 0 ? <EmptyState icon={<Server />} title={t('ops.system.noAdapters')} /> : (
              <ul className="flex flex-wrap gap-3 [&>li]:w-full sm:[&>li]:w-[22rem]">{adapters.map((a) => <AdapterCard key={a.node} adapter={a} />)}</ul>
            )}
          </Card>
        </div>
      )}
    </>
  )
}
