import { ArrowUpDown, BatteryCharging, DoorOpen } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { Link } from 'react-router'
import { useInfrastructure } from '../../api/queries'
import { Card, CardHeader } from '../../components/ui/Card'
import { EmptyState, ErrorState, Skeleton } from '../../components/ui/States'

/** Chargers, doors and lifts as the nav graph declares them. */
export function InfrastructurePanel() {
  const { t } = useTranslation()
  const infra = useInfrastructure()
  const head = 'px-3 py-2.5 text-left text-xs font-semibold tracking-wide text-slate-500 uppercase'
  return (
    <>
      {infra.isPending && <Skeleton className="h-80 rounded-2xl" />}
      {infra.isError && <Card><ErrorState onRetry={() => void infra.refetch()} /></Card>}
      {infra.data && (
        <div className="flex flex-col gap-4 lg:gap-6">
          <Card className="p-4 sm:p-5">
            <CardHeader icon={<BatteryCharging />} title={t('admin.infra.chargers')} subtitle={t('admin.infra.source', { path: infra.data.source })} className="mb-3" />
            {infra.data.chargers.length === 0 ? <EmptyState icon={<BatteryCharging />} title={t('admin.infra.noChargers')} /> : (
              <div className="-mx-4 overflow-x-auto sm:mx-0">
                <table className="w-full min-w-[520px] text-sm">
                  <thead className="border-b border-slate-200"><tr>
                    <th className={head}>{t('admin.infra.name')}</th><th className={head}>{t('admin.robots.map')}</th>
                    <th className={head}>{t('admin.robots.position')}</th><th className={head}>{t('admin.infra.usedBy')}</th>
                  </tr></thead>
                  <tbody className="divide-y divide-slate-100">
                    {infra.data.chargers.map((c) => (
                      <tr key={`${c.level}-${c.name}`}>
                        <td className="px-3 py-2.5 font-semibold text-slate-900">{c.name}</td>
                        <td className="px-3 py-2.5 text-slate-600">{c.level}</td>
                        <td className="px-3 py-2.5 text-slate-600">x {c.x.toFixed(2)} · y {c.y.toFixed(2)}</td>
                        <td className="px-3 py-2.5">{c.usedBy
                          ? <Link to={`/fleet/${encodeURIComponent(c.usedBy)}`} className="font-medium text-brand-700 hover:underline">{c.usedBy}</Link>
                          : <span className="text-emerald-700">{t('admin.fleets.free')}</span>}</td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
            )}
            <p className="mt-3 text-xs text-slate-500">{t('admin.infra.chargersHint')}</p>
          </Card>
          <div className="grid gap-4 lg:grid-cols-2 lg:gap-6">
            <Card className="p-4 sm:p-5">
              <CardHeader icon={<DoorOpen />} title={t('admin.infra.doors')} className="mb-3" />
              {infra.data.doors.length === 0 ? <p className="text-sm text-slate-500">{t('admin.infra.noDoors')}</p> : (
                <ul className="divide-y divide-slate-100 text-sm">
                  {infra.data.doors.map((d) => <li key={`${d.level}-${d.name}`} className="flex gap-3 py-2"><span className="font-semibold text-slate-900">{d.name}</span><span className="text-slate-500">{d.level} · {d.type || '—'}</span></li>)}
                </ul>
              )}
            </Card>
            <Card className="p-4 sm:p-5">
              <CardHeader icon={<ArrowUpDown />} title={t('admin.infra.lifts')} className="mb-3" />
              {infra.data.lifts.length === 0 ? <p className="text-sm text-slate-500">{t('admin.infra.noLifts')}</p> : (
                <ul className="divide-y divide-slate-100 text-sm">
                  {infra.data.lifts.map((l) => <li key={l.name} className="flex gap-3 py-2"><span className="font-semibold text-slate-900">{l.name}</span><span className="text-slate-500">{l.levels.join(', ')}</span></li>)}
                </ul>
              )}
            </Card>
          </div>
        </div>
      )}
    </>
  )
}
