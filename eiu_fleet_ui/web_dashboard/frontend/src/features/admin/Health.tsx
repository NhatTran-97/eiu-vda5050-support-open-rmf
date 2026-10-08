import { ChevronDown } from 'lucide-react'
import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import type { HealthItem, HealthStatus } from '../../api/types'
import { cn } from '../../lib/cn'
import { duration } from '../../lib/duration'

export const healthDot: Record<HealthStatus, string> = {
  ok: 'bg-emerald-500',
  warning: 'bg-amber-500',
  critical: 'bg-red-500',
  unknown: 'bg-slate-400',
}

const healthText: Record<HealthStatus, string> = {
  ok: 'text-emerald-700',
  warning: 'text-amber-700',
  critical: 'text-red-700',
  unknown: 'text-slate-500',
}

function DetailValue({ name, value }: { name: string; value: string | number | boolean | null }) {
  const { t } = useTranslation()
  if (value === null || value === '') return <>—</>
  if (typeof value === 'boolean') return <>{t(value ? 'admin.yes' : 'admin.no')}</>
  if (name.endsWith('_s') && typeof value === 'number') return <>{duration(value)}</>
  return <span className="break-all">{String(value)}</span>
}

/** One part of the system: status dot, name, status word; the figures behind it open on click. */
export function HealthRow({ item, expandable }: { item: HealthItem; expandable?: boolean }) {
  const { t } = useTranslation()
  const [open, setOpen] = useState(false)
  const details = Object.entries(item.details ?? {})
  const row = (
    <>
      <span className={cn('size-2.5 shrink-0 rounded-full', healthDot[item.status])} aria-hidden />
      <span className="min-w-0 flex-1 truncate text-sm text-slate-800">{t(`ops.health.${item.key}`, item.params)}</span>
      <span className={cn('text-sm font-semibold', healthText[item.status])}>{t(`ops.healthStatus.${item.status}`)}</span>
    </>
  )
  if (!expandable || details.length === 0) return <li className="flex items-center gap-3 py-2.5">{row}</li>
  return (
    <li>
      <button type="button" aria-expanded={open} onClick={() => setOpen((v) => !v)}
        className="flex w-full items-center gap-3 rounded-lg py-2.5 text-left hover:bg-slate-50">
        {row}
        <ChevronDown className={cn('size-4 shrink-0 text-slate-400 transition-transform', open && 'rotate-180')} />
      </button>
      {open && (
        <dl className="mb-2 ml-5 grid grid-cols-[auto_1fr] gap-x-6 gap-y-1.5 rounded-lg bg-slate-50 p-3 text-sm">
          {details.map(([name, value]) => (
            <div key={name} className="contents">
              <dt className="text-slate-500">{t(`admin.healthDetail.${name}`)}</dt>
              <dd className="text-slate-800"><DetailValue name={name} value={value} /></dd>
            </div>
          ))}
        </dl>
      )}
    </li>
  )
}

export function HealthList({ items, expandable }: { items: HealthItem[]; expandable?: boolean }) {
  return (
    <ul className="divide-y divide-slate-100">
      {items.map((h, i) => <HealthRow key={`${h.key}-${i}`} item={h} expandable={expandable} />)}
    </ul>
  )
}
