import type { HTMLAttributes, ReactNode } from 'react'
import { cn } from '../../lib/cn'

export function Card({ className, ...props }: HTMLAttributes<HTMLElement>) {
  return <section className={cn('rounded-xl border border-ops-border bg-ops-card shadow-ops', className)} {...props} />
}

interface CardHeaderProps {
  icon?: ReactNode
  title: ReactNode
  subtitle?: ReactNode
  action?: ReactNode
  className?: string
  as?: 'h2' | 'h3'
}

export function CardHeader({ icon, title, subtitle, action, className, as: Heading = 'h2' }: CardHeaderProps) {
  return (
    <div className={cn('flex items-center gap-3', className)}>
      {icon && <span className="flex shrink-0 text-ops-muted [&>svg]:size-5 [&>svg]:stroke-[1.75]" aria-hidden>{icon}</span>}
      <div className="min-w-0 flex-1">
        <Heading className="truncate text-section font-semibold text-ops-text">{title}</Heading>
        {subtitle && <p className="mt-0.5 text-meta text-ops-muted">{subtitle}</p>}
      </div>
      {action}
    </div>
  )
}
