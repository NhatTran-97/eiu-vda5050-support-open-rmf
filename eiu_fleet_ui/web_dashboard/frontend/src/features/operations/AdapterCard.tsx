import { Server } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import type { AdapterSystem } from '../../api/types'
import { cn } from '../../lib/cn'
import { duration } from '../../lib/duration'

const statusTone: Record<AdapterSystem['status'], string> = {
  ok: 'bg-emerald-50 text-emerald-700',
  warning: 'bg-amber-50 text-amber-700',
  critical: 'bg-red-50 text-red-700',
  silent: 'bg-red-50 text-red-700',
  waiting: 'bg-slate-100 text-slate-600',
  found: 'bg-slate-100 text-slate-600',
  absent: 'bg-slate-100 text-slate-500',
}


/** Line of the last samples scaled to their range; gaps (null) break the line. */
export function Sparkline({ values, className }: { values: (number | null)[]; className?: string }) {
  const known = values.filter((v): v is number => v !== null)
  if (known.length < 2) return <div className={cn('h-10', className)} />
  const max = Math.max(...known)
  const min = Math.min(...known)
  const span = max - min || 1
  const w = 120
  const h = 32
  const step = w / Math.max(values.length - 1, 1)
  let d = ''
  values.forEach((v, i) => {
    if (v === null) return
    const x = i * step
    const y = h - ((v - min) / span) * (h - 2) - 1
    d += `${d && values[i - 1] !== null ? 'L' : 'M'}${x.toFixed(1)},${y.toFixed(1)}`
  })
  return (
    <svg viewBox={`0 0 ${w} ${h}`} preserveAspectRatio="none" className={cn('h-10 w-full text-brand-500', className)} aria-hidden>
      <path d={d} fill="none" stroke="currentColor" strokeWidth={1.5} vectorEffect="non-scaling-stroke" />
    </svg>
  )
}

export function AdapterCard({ adapter }: { adapter: AdapterSystem }) {
  const { t } = useTranslation()
  const rate = adapter.series.msg_per_s
  const lastRate = [...rate].reverse().find((v) => v !== null)
  return (
    <li className="flex min-w-0 flex-col gap-3 rounded-xl p-4 ring-1 ring-slate-200">
      <div className="flex items-start gap-3">
        <span className="flex size-10 shrink-0 items-center justify-center rounded-full bg-brand-50 text-brand-600"><Server className="size-5" /></span>
        <div className="min-w-0 flex-1">
          <p className="truncate font-bold text-slate-900">{adapter.fleet || adapter.node}</p>
          <p className="truncate text-sm text-slate-500">{adapter.node}</p>
        </div>
        <span className={cn('rounded-full px-2.5 py-1 text-xs font-semibold', statusTone[adapter.status])}>{t(`ops.adapterStatus.${adapter.status}`)}</span>
      </div>
      {adapter.reported_ago_s === null ? (
        <p className="text-sm text-slate-500">{t('ops.system.waiting')}</p>
      ) : (
        <>
          <dl className="grid grid-cols-2 gap-x-4 gap-y-1 text-sm">
            <dd className="col-span-2 text-slate-600">{t('ops.system.robots', adapter.robots)}</dd>
            <dd className={adapter.mqtt.connected ? 'text-emerald-700' : 'text-red-700'}>
              {adapter.mqtt.connected ? t('ops.system.mqttOk') : t('ops.system.mqttLost')}
            </dd>
            <dd className="text-slate-600">{t('ops.system.uptime', { time: duration(adapter.uptime_s) })}</dd>
            <dd className="text-slate-600">{t('ops.system.rx', { count: adapter.totals.rx })}</dd>
            <dd className={adapter.totals.dropped > 0 ? 'text-amber-700' : 'text-slate-600'}>{t('ops.system.dropped', { count: adapter.totals.dropped })}</dd>
          </dl>
          <div>
            <p className="flex justify-between text-xs text-slate-500">
              <span>{t('ops.system.rate')}</span>
              <span>{lastRate !== undefined ? lastRate.toFixed(1) : '—'}</span>
            </p>
            <Sparkline values={rate} />
          </div>
          <p className="text-xs text-slate-500">
            {t('ops.system.reported', { ago: duration(adapter.reported_ago_s), interval: duration(adapter.interval_s) })}
          </p>
        </>
      )}
    </li>
  )
}
