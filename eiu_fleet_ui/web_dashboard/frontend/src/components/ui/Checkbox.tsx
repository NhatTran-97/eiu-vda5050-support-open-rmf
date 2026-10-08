import type { ReactNode } from 'react'
import { cn } from '../../lib/cn'

export function Checkbox({ checked, onChange, label, hint, disabled, className }: {
  checked: boolean
  onChange: (checked: boolean) => void
  label: ReactNode
  hint?: ReactNode
  disabled?: boolean
  className?: string
}) {
  return (
    <label className={cn('flex cursor-pointer items-start gap-3 rounded-lg px-2 py-1.5 hover:bg-slate-50', disabled && 'cursor-not-allowed opacity-60', className)}>
      <input type="checkbox" checked={checked} disabled={disabled} onChange={(e) => onChange(e.target.checked)}
        className="mt-0.5 size-4 shrink-0 rounded accent-brand-600" />
      <span className="min-w-0">
        <span className="block text-sm font-medium text-slate-800">{label}</span>
        {hint && <span className="block text-xs text-slate-500">{hint}</span>}
      </span>
    </label>
  )
}
