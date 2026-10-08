import { CircleAlert, Info, OctagonAlert, type LucideIcon } from 'lucide-react'
import { useCallback } from 'react'
import { useTranslation } from 'react-i18next'
import type { AlertSeverity, OpsAlert } from '../../api/types'
import { cn } from '../../lib/cn'

export const severityStyle: Record<AlertSeverity, { icon: LucideIcon; chip: string; row: string }> = {
  critical: { icon: OctagonAlert, chip: 'bg-red-50 text-red-700 ring-red-200', row: 'border-l-red-500' },
  warning: { icon: CircleAlert, chip: 'bg-amber-50 text-amber-700 ring-amber-200', row: 'border-l-amber-500' },
  info: { icon: Info, chip: 'bg-slate-100 text-slate-700 ring-slate-200', row: 'border-l-slate-400' },
}

/** Severity as icon, color and word, so it does not depend on color alone. */
export function SeverityChip({ severity, className }: { severity: AlertSeverity; className?: string }) {
  const { t } = useTranslation()
  const { icon: Icon, chip } = severityStyle[severity]
  return (
    <span className={cn('inline-flex h-6 shrink-0 items-center gap-1 rounded-full px-2 text-xs font-semibold ring-1 ring-inset', chip, className)}>
      <Icon className="size-3.5" aria-hidden />
      {t(`alerts.severity.${severity}`)}
    </span>
  )
}

/** Title and detail of an alert in the current language. */
export function useAlertText() {
  const { t, i18n } = useTranslation()
  return useCallback((alert: OpsAlert) => {
    const p = alert.params
    if (alert.code === 'adapter.attention') return { title: String(p.title ?? ''), detail: String(p.detail ?? '') }
    const key = `alerts.code.${alert.code}`
    if (!i18n.exists(`${key}.title`)) return { title: alert.code, detail: '' }
    const error = p.error ? String(p.error) : ''
    const errorText = error && i18n.exists(`errors.${error}`) ? t(`errors.${error}`) : error
    return { title: t(`${key}.title`, p), detail: t(`${key}.detail`, { ...p, error: errorText }) }
  }, [t, i18n])
}
