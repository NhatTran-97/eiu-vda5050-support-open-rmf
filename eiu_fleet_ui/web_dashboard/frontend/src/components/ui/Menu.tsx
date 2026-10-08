import { useEffect, useRef, useState, type ReactNode } from 'react'
import { Link } from 'react-router'
import { cn } from '../../lib/cn'

interface MenuProps {
  trigger: (props: { open: boolean; toggle: () => void }) => ReactNode
  children: (close: () => void) => ReactNode
  align?: 'start' | 'end'
  className?: string
}

/** Popup anchored to its trigger; closes on outside click and Escape. */
export function Menu({ trigger, children, align = 'end', className }: MenuProps) {
  const [open, setOpen] = useState(false)
  const root = useRef<HTMLDivElement>(null)

  useEffect(() => {
    if (!open) return
    const onPointer = (e: PointerEvent) => {
      if (!root.current?.contains(e.target as Node)) setOpen(false)
    }
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') setOpen(false)
    }
    document.addEventListener('pointerdown', onPointer)
    document.addEventListener('keydown', onKey)
    return () => {
      document.removeEventListener('pointerdown', onPointer)
      document.removeEventListener('keydown', onKey)
    }
  }, [open])

  const close = () => setOpen(false)
  return (
    <div ref={root} className="relative">
      {trigger({ open, toggle: () => setOpen((o) => !o) })}
      {open && (
        <div
          className={cn(
            'absolute top-full z-50 mt-2 min-w-52 overflow-hidden rounded-xl bg-surface py-1.5 text-sm text-slate-700 shadow-xl ring-1 ring-slate-900/10',
            align === 'end' ? 'right-0' : 'left-0',
            className,
          )}
        >
          {children(close)}
        </div>
      )}
    </div>
  )
}

const itemClass = 'flex w-full items-center gap-2.5 px-3.5 py-2.5 text-left hover:bg-slate-50 [&>svg]:size-4 [&>svg]:text-slate-400'

export function MenuItem({ onClick, children, danger }: { onClick: () => void; children: ReactNode; danger?: boolean }) {
  return (
    <button type="button" onClick={onClick} className={cn(itemClass, danger && 'text-red-600 [&>svg]:text-red-500')}>
      {children}
    </button>
  )
}

export function MenuLink({ to, onClick, children }: { to: string; onClick?: () => void; children: ReactNode }) {
  return (
    <Link to={to} onClick={onClick} className={itemClass}>
      {children}
    </Link>
  )
}

export function MenuSeparator() {
  return <div className="my-1.5 h-px bg-slate-100" />
}
