import type { ReactNode } from 'react'
import { cn } from '../../lib/cn'

export interface TabItem<T extends string> {
  value: T
  label: string
  icon?: ReactNode
  count?: number
}

interface TabsProps<T extends string> {
  value: T
  onChange: (value: T) => void
  items: TabItem<T>[]
  label: string
  className?: string
}

/** Segmented tabs; scrolls sideways when the row is wider than the screen. */
export function Tabs<T extends string>({ value, onChange, items, label, className }: TabsProps<T>) {
  return (
    <div role="tablist" aria-label={label} className={cn('scrollbar-none flex gap-1 overflow-x-auto', className)}>
      {items.map((item) => {
        const selected = item.value === value
        return (
          <button
            key={item.value}
            type="button"
            role="tab"
            aria-selected={selected}
            onClick={() => onChange(item.value)}
            className={cn(
              'flex h-11 shrink-0 items-center gap-2 rounded-xl px-4 text-sm font-semibold transition-colors sm:px-6',
              selected ? 'bg-brand-600 text-white' : 'text-slate-600 hover:bg-slate-100',
            )}
          >
            {item.icon}
            <span>{item.label}</span>
            {item.count !== undefined && <span className={selected ? 'text-white/80' : 'text-slate-400'}>({item.count})</span>}
          </button>
        )
      })}
    </div>
  )
}
