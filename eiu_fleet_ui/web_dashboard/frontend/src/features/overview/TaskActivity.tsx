import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import { useActivity } from '../../api/queries'
import { FilterTabs } from '../../components/ui/FilterTabs'
import { ErrorState, Skeleton } from '../../components/ui/States'
import { useAllowedServices } from '../../domain/access'
import { useServiceName } from '../../domain/services'
import { cn } from '../../lib/cn'
import { useFormat } from '../../lib/format'
import { ChartFrame, MultiLine, type Series } from '../charts/Charts'

type Range = 'today' | 'week' | 'month'

const CHART_HEIGHT = 120

/**
 * Overview summary of task activity: four totals of the period (created, running now, completed, failed) and a short
 * chart per hour (today) or per day. The full history is on the Analytics page.
 */
export function TaskActivity() {
  const { t } = useTranslation()
  const format = useFormat()
  const services = useAllowedServices()
  const serviceName = useServiceName()
  const [range, setRange] = useState<Range>('today')
  const [service, setService] = useState('')
  const activity = useActivity(range, service)
  const series: Series[] = [
    { key: 'created', label: t('activity.created'), color: 'var(--color-status-active)' },
    { key: 'running', label: t('activity.running'), color: 'var(--color-status-live)' },
    { key: 'completed', label: t('activity.completed'), color: 'var(--color-status-success)' },
    { key: 'failed', label: t('activity.failed'), color: 'var(--color-status-critical)' },
  ]
  // Hours of today that have not begun yet carry no data; they are left off the chart.
  const buckets = (activity.data?.buckets ?? []).filter((b) => b.start <= Date.now())
  const label = (ms: number) => (activity.data?.unit === 'hour' ? format.time(ms) : format.shortDate(ms))
  const values = Object.fromEntries(series.map((s) => [s.key, buckets.map((b) => b[s.key as 'created'])]))
  // Created, completed and failed add up over the period; running is the count in the latest bucket.
  const totals: Record<string, number> = {
    created: buckets.reduce((n, b) => n + b.created, 0),
    running: buckets[buckets.length - 1]?.running ?? 0,
    completed: buckets.reduce((n, b) => n + b.completed, 0),
    failed: buckets.reduce((n, b) => n + b.failed, 0),
  }
  return (
    <div className="flex flex-col gap-3">
      <div className="flex flex-wrap items-center gap-2">
        <FilterTabs<Range> size="sm" value={range} onChange={setRange} label={t('activity.range')}
          items={(['today', 'week', 'month'] as const).map((r) => ({ value: r, label: t(`activity.${r}`) }))} />
        {services.length > 1 && (
          <select value={service} onChange={(e) => setService(e.target.value)} aria-label={t('filters.service')}
            className="h-8 rounded-full border border-ops-border bg-ops-card px-2.5 text-control font-medium text-ops-text">
            <option value="">{t('filters.allServices')}</option>
            {services.map((s) => <option key={s} value={s}>{serviceName(s)}</option>)}
          </select>
        )}
      </div>
      {/* Four equal columns; each label/value group is centered in its column and left-aligned inside. */}
      <dl className="grid grid-cols-4 gap-2">
        {series.map((s) => (
          <div key={s.key} className="flex min-w-0 justify-center">
            <div className="min-w-0">
              <dt className="flex items-center gap-1.5 whitespace-nowrap text-body text-ops-muted"><span className="size-2 shrink-0 rounded-sm" style={{ background: s.color }} aria-hidden />{s.label}</dt>
              <dd className={cn('mt-1 text-kpi-secondary font-semibold tabular-nums',
                s.key === 'failed' && totals.failed > 0 ? 'text-ops-red' : 'text-ops-text')}>{activity.data ? totals[s.key] : '—'}</dd>
            </div>
          </div>
        ))}
      </dl>
      {activity.isPending && <Skeleton className="h-28" />}
      {activity.isError && <ErrorState onRetry={() => void activity.refetch()} />}
      {activity.data && (
        <ChartFrame table={{ head: [t('activity.when'), ...series.map((s) => s.label)], rows: buckets.map((b) => [label(b.start), b.created, b.running, b.completed, b.failed]) }}>
          <MultiLine height={CHART_HEIGHT} endLabels={false} labels={buckets.map((b) => label(b.start))} series={series} values={values} empty={t('activity.empty')} />
        </ChartFrame>
      )}
    </div>
  )
}
