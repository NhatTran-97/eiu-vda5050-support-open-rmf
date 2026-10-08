import { Map as MapIcon, MapPinned, Route, Shapes, Sparkles } from 'lucide-react'
import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import { useCatalog, useLocations, useToggle } from '../../api/queries'
import { Card, CardHeader } from '../../components/ui/Card'
import { Skeleton } from '../../components/ui/States'
import { Switch } from '../../components/ui/Switch'
import { Tabs } from '../../components/ui/Tabs'
import { InfrastructurePanel } from '../../features/locations/InfrastructurePanel'
import { MapsPanel } from '../../features/locations/MapsPanel'
import { PageHeader } from '../../layout/PageHeader'
import { useErrorText } from '../../lib/errors'
import { useLocalize } from '../../lib/i18nText'
import { toast } from '../../lib/toast'

type Tab = 'catalog' | 'map' | 'infrastructure'

function Catalog() {
  const { t } = useTranslation()
  const localize = useLocalize()
  const catalog = useCatalog().data
  const locations = useLocations().data
  const toggle = useToggle()
  const errorText = useErrorText()
  if (!catalog || !locations) return <Skeleton className="h-80 rounded-2xl" />
  const name = (id: string) => localize(locations.find((l) => l.id === id)?.name ?? { vi: id, en: id })
  return (
    <div className="flex flex-col gap-4 lg:gap-6">
      {catalog.zones.map((z) => {
        const places = locations.filter((l) => l.zone === z.id)
        return (
          <Card key={z.id} className="p-4 sm:p-5">
            <CardHeader icon={<Shapes />} title={localize(z.name)} subtitle={`${z.id} · ${z.levelId}`} className="mb-4"
              action={<div className="shrink-0"><Switch label={t('locations.zoneOpen')} checked={z.enabled} disabled={toggle.isPending}
                onChange={(enabled) => toggle.mutate({ kind: 'zone', id: z.id, enabled }, { onError: (e) => toast.error(errorText(e)) })} /></div>} />
            <div className="grid gap-4 lg:grid-cols-3">
              <section>
                <h3 className="mb-2 flex items-center gap-1.5 text-sm font-semibold text-slate-800"><MapPinned className="size-4 text-slate-400" />{t('locations.places', { count: places.length })}</h3>
                <ul className="flex flex-col gap-0.5 text-sm">{places.map((l) => <li key={l.id} className="flex items-baseline justify-between gap-2"><span className="text-slate-800">{localize(l.name)}</span><span className="text-meta text-slate-500">{l.waypoint}</span></li>)}</ul>
              </section>
              <section>
                <h3 className="mb-2 flex items-center gap-1.5 text-sm font-semibold text-slate-800"><Sparkles className="size-4 text-slate-400" />{t('locations.areas')}</h3>
                <ul className="flex flex-col gap-0.5 text-sm">{catalog.areas.filter((a) => a.zone === z.id).map((a) => <li key={a.id} className="flex items-baseline justify-between gap-2"><span className="text-slate-800">{localize(a.name)}</span><span className="text-meta text-slate-500">{a.rmfZone}</span></li>)}</ul>
              </section>
              <section>
                <h3 className="mb-2 flex items-center gap-1.5 text-sm font-semibold text-slate-800"><Route className="size-4 text-slate-400" />{t('locations.routes')}</h3>
                <ul className="flex flex-col gap-1.5 text-sm">{catalog.routes.filter((r) => r.zone === z.id).map((r) => <li key={r.id}><span className="font-medium text-slate-800">{localize(r.name)}</span><span className="block text-meta text-slate-500">{r.stops.map(name).join(' → ')}</span></li>)}</ul>
              </section>
            </div>
          </Card>
        )
      })}
      <p className="mt-2 text-meta text-slate-500">{t('locations.catalogHint')}</p>
    </div>
  )
}

/** Locations: zones (open or closed to robot tasks), places, cleaning areas, patrol routes, the map and the infrastructure. */
export default function LocationsPage() {
  const { t } = useTranslation()
  const [tab, setTab] = useState<Tab>('catalog')
  return (
    <>
      <PageHeader title={t('locations.title')} subtitle={t('locations.subtitle')} />
      <Tabs<Tab> value={tab} onChange={setTab} label={t('locations.title')} className="mb-4 rounded-2xl bg-surface p-1.5 shadow-card ring-1 ring-slate-900/5 lg:mb-6"
        items={[{ value: 'catalog', label: t('locations.tabCatalog'), icon: <Shapes className="size-4" /> },
          { value: 'map', label: t('locations.tabMap'), icon: <MapIcon className="size-4" /> },
          { value: 'infrastructure', label: t('locations.tabInfrastructure'), icon: <MapPinned className="size-4" /> }]} />
      {tab === 'catalog' && <Catalog />}
      {tab === 'map' && <MapsPanel />}
      {tab === 'infrastructure' && <InfrastructurePanel />}
    </>
  )
}
