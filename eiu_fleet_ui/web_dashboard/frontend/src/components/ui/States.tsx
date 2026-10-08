import { CircleAlert, LoaderCircle } from 'lucide-react'
import type { ReactNode } from 'react'
import { useTranslation } from 'react-i18next'
import { cn } from '../../lib/cn'
import { Button } from './Button'

export function Skeleton({ className }: { className?: string }) {
  return <div className={cn('animate-pulse rounded-xl bg-slate-200/70', className)} />
}

export function Spinner({ className }: { className?: string }) {
  return <LoaderCircle className={cn('size-6 animate-spin text-brand-500', className)} aria-hidden />
}

export function EmptyState({ icon, title, body, action, className }: {
  icon: ReactNode
  title: ReactNode
  body?: ReactNode
  action?: ReactNode
  className?: string
}) {
  return (
    <div className={cn('flex flex-col items-center px-6 py-10 text-center', className)}>
      <span className="flex size-14 items-center justify-center rounded-full bg-brand-50 text-brand-600 [&>svg]:size-7">{icon}</span>
      <p className="mt-4 text-base font-bold text-slate-900">{title}</p>
      {body && <p className="mt-1 max-w-sm text-sm text-slate-500">{body}</p>}
      {action && <div className="mt-5">{action}</div>}
    </div>
  )
}

export function ErrorState({ onRetry, className }: { onRetry?: () => void; className?: string }) {
  const { t } = useTranslation()
  return (
    <EmptyState
      className={className}
      icon={<CircleAlert />}
      title={t('common.loadFailed')}
      action={onRetry && <Button variant="secondary" onClick={onRetry}>{t('common.retry')}</Button>}
    />
  )
}
