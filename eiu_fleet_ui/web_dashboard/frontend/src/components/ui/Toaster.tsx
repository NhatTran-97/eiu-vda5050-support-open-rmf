import { CircleAlert, CircleCheck, Info, X } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { cn } from '../../lib/cn'
import { useToasts } from '../../lib/toast'

const icons = { success: CircleCheck, error: CircleAlert, info: Info }
const tones = { success: 'text-emerald-500', error: 'text-red-500', info: 'text-brand-500' }

export function Toaster() {
  const toasts = useToasts((s) => s.toasts)
  const dismiss = useToasts((s) => s.dismiss)
  const { t } = useTranslation()
  return (
    <div aria-live="polite" className="pointer-events-none fixed inset-x-0 bottom-24 z-[60] flex flex-col items-center gap-2 px-4 lg:bottom-6">
      {toasts.map((toast) => {
        const Icon = icons[toast.kind]
        return (
          <div key={toast.id} className="pointer-events-auto flex w-full max-w-sm items-center gap-3 rounded-xl bg-surface px-4 py-3 text-sm font-medium text-slate-800 shadow-xl ring-1 ring-slate-900/10">
            <Icon className={cn('size-5 shrink-0', tones[toast.kind])} aria-hidden />
            <span className="min-w-0 flex-1">{toast.text}</span>
            <button type="button" aria-label={t('common.close')} onClick={() => dismiss(toast.id)} className="text-slate-400 hover:text-slate-700">
              <X className="size-4" />
            </button>
          </div>
        )
      })}
    </div>
  )
}
