import type { ReactNode } from 'react'
import { useTranslation } from 'react-i18next'
import { RobotIllustration } from '../assets/RobotArt'
import { cn } from '../lib/cn'

interface PageHeaderProps {
  title: ReactNode
  subtitle?: ReactNode
  hero?: boolean
  actions?: ReactNode
  before?: ReactNode
}

/** Page title, short subtitle and the page's primary actions, as on the Overview. The hero variant adds the robot
 * artwork on wide screens. */
export function PageHeader({ title, subtitle, hero, actions, before }: PageHeaderProps) {
  const { t } = useTranslation()
  return (
    <header className={cn('relative flex flex-wrap items-end gap-x-6 gap-y-2 pt-5 pb-4', hero && 'overflow-x-clip md:min-h-48 lg:min-h-52')}>
      <div className="relative z-10 min-w-0 flex-1">
        {before}
        <h1 className="text-page-title font-semibold tracking-tight text-ops-text">{title}</h1>
        {subtitle && <p className="mt-1 max-w-3xl text-card-title text-ops-muted">{subtitle}</p>}
      </div>
      {actions && <div className="relative z-10 flex shrink-0 flex-wrap gap-2">{actions}</div>}
      {hero && (
        <div aria-hidden className="pointer-events-none absolute inset-y-0 right-0 hidden items-end gap-6 md:flex">
          <div className="relative h-full">
            <div className="absolute inset-x-[-40%] bottom-2 h-1/2 rounded-full bg-brand-500/30 blur-3xl" />
            <RobotIllustration className="relative h-full max-h-52 w-auto py-2" />
          </div>
          <p className="hidden w-28 pb-6 text-[13px] leading-6 font-semibold tracking-[0.2em] text-brand-200 uppercase xl:block">
            {t('app.tagline').split(' · ').map((word) => <span key={word} className="block">{word}</span>)}
            <span className="mt-2 block h-0.5 w-8 bg-brand-300" />
          </p>
        </div>
      )}
    </header>
  )
}
