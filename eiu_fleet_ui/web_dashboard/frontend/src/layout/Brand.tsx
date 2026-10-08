import { useTranslation } from 'react-i18next'
import { Link } from 'react-router'
import { LogoMark } from '../assets/RobotArt'
import { cn } from '../lib/cn'

/** "EIU Robot Services" and the platform line under it. */
export function Brand({ compact, light, className }: { compact?: boolean; light?: boolean; className?: string }) {
  const { t } = useTranslation()
  return (
    <Link to="/overview" className={cn('flex items-center gap-3', light ? 'text-eiu-navy dark:text-white' : 'text-white', className)}>
      <LogoMark className={compact ? 'size-9' : 'size-11'} />
      <span className="min-w-0 leading-tight whitespace-nowrap">
        <span className={cn('block font-extrabold tracking-tight', compact && 'truncate', compact ? 'text-base' : 'text-xl')}>{t('app.brand')}</span>
        <span className={cn('block font-medium', compact && 'truncate', light ? 'text-ops-muted' : 'text-brand-200 dark:text-white/65', compact ? 'text-[11px]' : 'text-xs')}>{t('app.platform')}</span>
      </span>
    </Link>
  )
}
