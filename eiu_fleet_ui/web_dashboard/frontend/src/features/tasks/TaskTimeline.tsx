import { useTranslation } from 'react-i18next'
import type { TaskActivityEntry } from '../../api/types'
import { cn } from '../../lib/cn'
import { useFormat } from '../../lib/format'

const CRITICAL = new Set(['failed'])
const WARNING = new Set(['delayed'])

/** What happened to a task, oldest first: time, event, detail. */
export function TaskTimeline({ entries, className }: { entries: TaskActivityEntry[]; className?: string }) {
  const { t, i18n } = useTranslation()
  const format = useFormat()
  if (entries.length === 0) return <p className="text-sm text-slate-500">{t('tasks.noActivity')}</p>
  return (
    <ol className={cn('relative flex flex-col gap-3 border-l-2 border-slate-200 pl-5', className)}>
      {entries.map((e, i) => {
        const key = `tasks.event.${e.type.replace('.', '_')}`
        const last = i === entries.length - 1
        return (
          <li key={`${e.at}-${i}`} className="relative">
            <span className={cn('absolute top-1.5 -left-[27px] size-3 rounded-full ring-4 ring-surface',
              CRITICAL.has(e.type) ? 'bg-red-500' : WARNING.has(e.type) ? 'bg-amber-500' : last ? 'bg-brand-600' : 'bg-slate-400')} />
            <p className="text-meta text-slate-500 tabular-nums">{format.time(e.at)}</p>
            <p className={cn('text-sm', CRITICAL.has(e.type) ? 'font-semibold text-red-700' : last ? 'font-semibold text-slate-900' : 'text-slate-700')}>
              {i18n.exists(key) ? t(key, { detail: e.detail }) : e.type}
            </p>
            {e.detail && !i18n.exists(key) && <p className="text-xs text-slate-500">{e.detail}</p>}
          </li>
        )
      })}
    </ol>
  )
}
