import type { ReactNode } from 'react'
import { cn } from '../../lib/cn'

export interface FilterItem<T extends string> {
  value: T
  label: string
  icon?: ReactNode
  count?: number
}

/** Pill filter row; scrolls sideways on narrow screens. `muteZero` softens unselected items whose count is 0 and sets
 * non-zero counts bolder; every item stays visible and keeps its size. */
export function FilterTabs<T extends string>({ value, onChange, items, label, size = 'md', muteZero, className }: {
  value: T
  onChange: (value: T) => void
  items: FilterItem<T>[]
  label: string
  size?: 'sm' | 'md'
  muteZero?: boolean
  className?: string
}) {
  return (
    <div role="radiogroup" aria-label={label} className={cn('scrollbar-none flex gap-1.5 overflow-x-auto', className)}>
      {items.map((item) => {
        const selected = item.value === value
        const muted = muteZero && !selected && item.count === 0
        return (
          <button
            key={item.value}
            type="button"
            role="radio"
            aria-checked={selected}
            onClick={() => onChange(item.value)}
            className={cn(
              'flex shrink-0 items-center gap-1.5 rounded-full font-semibold ring-1 transition-colors ring-inset',
              size === 'sm' ? 'h-8 px-3 text-control' : 'h-9 px-3.5 text-control',
              selected ? 'bg-brand-600 text-white ring-brand-600'
                : muted ? 'bg-surface text-slate-400 ring-slate-100 hover:bg-slate-50 hover:text-slate-700'
                  : 'bg-surface text-slate-600 ring-slate-200 hover:bg-slate-50 hover:text-slate-900',
            )}
          >
            {item.icon}
            <span>{item.label}</span>
            {item.count !== undefined && <span className={cn('tabular-nums', selected ? 'text-white/80' : muteZero && item.count > 0 ? 'font-bold text-slate-900' : 'text-slate-400')}>{item.count}</span>}
          </button>
        )
      })}
    </div>
  )
}
