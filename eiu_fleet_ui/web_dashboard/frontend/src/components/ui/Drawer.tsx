import { X } from 'lucide-react'
import { useEffect, useRef, type ReactNode } from 'react'
import { useTranslation } from 'react-i18next'
import { cn } from '../../lib/cn'
import { IconButton } from './Button'

const WIDTH = { compact: 'md:max-w-[18.5rem]', standard: 'md:max-w-xs', wide: 'md:max-w-[22rem]', form: 'md:max-w-lg' } as const

interface DrawerProps {
  open: boolean
  onClose: () => void
  title: ReactNode
  subtitle?: ReactNode
  children: ReactNode
  footer?: ReactNode
  /** `inline` (default) places the footer right after the content, scrolling with it; `pinned` keeps it at the bottom. */
  footerPlacement?: 'pinned' | 'inline'
  /** `fit` (default): as tall as its content, the body scrolling beyond the maximum; below md a bottom sheet (up to 85 % of
   * the screen, at most 24rem wide and centred), from md a floating panel below the top bar (min 24rem, max down to 1rem above the bottom).
   * `full`: a full-height panel on the right at every width. */
  size?: 'full' | 'fit'
  /** Width of the floating panel (md and up), independent of the viewport: compact 18.5rem, standard 20rem (default),
   * wide 22rem for content that needs it, form 32rem for editing panels. */
  width?: 'compact' | 'standard' | 'wide' | 'form'
  className?: string
}

/** Quick inspection panel on the native <dialog>: focus trap, Escape and backdrop click close it, and focus returns to the
 * element that opened it. The backdrop only dims lightly, so the robot selected on the map stays visible beside the panel.
 * It is as tall as its content with its actions right after it: a floating panel on the right from md, a bottom sheet on
 * phones. */
export function Drawer({ open, onClose, title, subtitle, children, footer, footerPlacement = 'inline', size = 'fit', width = 'standard', className }: DrawerProps) {
  const fit = size === 'fit'
  const ref = useRef<HTMLDialogElement>(null)
  const { t } = useTranslation()

  useEffect(() => {
    const dialog = ref.current
    if (!dialog) return
    if (open && !dialog.open) dialog.showModal()
    if (!open && dialog.open) dialog.close()
  }, [open])

  return (
    <dialog
      ref={ref}
      onClose={onClose}
      onCancel={(e) => {
        e.preventDefault()
        onClose()
      }}
      onClick={(e) => {
        if (e.target === ref.current) onClose()
      }}
      className={cn(
        'fixed m-0 bg-surface p-0 text-slate-900 shadow-2xl backdrop:bg-navy-950/20',
        fit
          // Phones: a centred sheet, as wide as the content needs (at most 24rem), never wider than the screen minus 1rem.
          ? cn('inset-x-0 top-auto bottom-0 mx-auto h-auto max-h-[85dvh] w-[calc(100%-1rem)] max-w-sm rounded-t-2xl ring-1 ring-slate-200',
            'md:inset-x-auto md:top-[calc(var(--topbar-h)+1rem)] md:right-4 md:bottom-auto md:mx-0 md:max-h-[calc(100dvh-var(--topbar-h)-2rem)] md:min-h-[24rem] md:w-full md:rounded-2xl',
            WIDTH[width])
          : 'inset-y-0 right-0 left-auto h-dvh max-h-dvh w-full max-w-md',
        className,
      )}
    >
      {open && (
        <div className={cn('flex h-full flex-col', fit && 'max-h-[85dvh] md:max-h-[calc(100dvh-var(--topbar-h)-2rem)]')}>
          <div className="flex items-start gap-3 border-b border-slate-100 px-4 py-4">
            <div className="min-w-0 flex-1">
              <h2 className="truncate text-lg font-bold">{title}</h2>
              {subtitle && <div className="mt-0.5 text-sm text-slate-500">{subtitle}</div>}
            </div>
            <IconButton label={t('common.close')} onClick={onClose} className="-mr-2">
              <X className="size-5" />
            </IconButton>
          </div>
          <div className="min-h-0 flex-1 overflow-y-auto px-4 py-4">
            {children}
            {footer && footerPlacement === 'inline' && <div className="mt-6 flex flex-wrap justify-end gap-2 border-t border-slate-100 pt-5">{footer}</div>}
          </div>
          {footer && footerPlacement === 'pinned' && <div className="flex flex-wrap justify-end gap-2 border-t border-slate-100 px-4 py-4">{footer}</div>}
        </div>
      )}
    </dialog>
  )
}

/** Label/value rows of a drawer or card. `quiet` is for supporting metadata: smaller, values in normal weight. */
export function Facts({ rows, quiet, className }: { rows: [ReactNode, ReactNode][]; quiet?: boolean; className?: string }) {
  return (
    <dl className={cn('grid grid-cols-[7.5rem_minmax(0,1fr)] gap-x-4', quiet ? 'gap-y-1.5 text-meta' : 'gap-y-2.5 text-sm', className)}>
      {rows.map(([label, value], i) => (
        <div key={i} className="contents">
          <dt className="text-slate-500">{label}</dt>
          <dd className={cn('min-w-0 break-words', quiet ? 'text-slate-600' : 'font-medium text-slate-900')}>{value ?? '—'}</dd>
        </div>
      ))}
    </dl>
  )
}
