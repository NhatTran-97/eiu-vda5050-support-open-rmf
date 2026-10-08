import { useTranslation } from 'react-i18next'
import { NavLink } from 'react-router'
import { cn } from '../lib/cn'
import { menuItem } from './nav'
import { CountBadge, useBadgeCounts, useNav } from './Sidebar'

/** Tab bar for phones and tablets; replaces the sidebar below the lg breakpoint. */
export function BottomNav() {
  const { t } = useTranslation()
  const counts = useBadgeCounts()
  const items = [...useNav().flatMap((g) => g.items).filter((item) => !item.desktopOnly).slice(0, 4), menuItem]
  return (
    <nav aria-label={t('nav.main')} className="fixed inset-x-0 bottom-0 z-40 border-t border-slate-200 bg-surface/95 pb-[env(safe-area-inset-bottom)] backdrop-blur lg:hidden">
      <ul className="mx-auto grid max-w-xl" style={{ gridTemplateColumns: `repeat(${items.length}, minmax(0, 1fr))` }}>
        {items.map((item) => {
          const Icon = item.icon
          return (
            <li key={item.to}>
              <NavLink to={item.to} end={item.end}
                className={({ isActive }) => cn('flex h-16 flex-col items-center justify-center gap-1 text-[11px] font-semibold transition-colors',
                  isActive ? 'text-brand-600' : 'text-slate-500 hover:text-slate-900')}>
                {({ isActive }) => (
                  <>
                    <span className={cn('relative flex h-7 w-12 items-center justify-center rounded-full transition-colors', isActive && 'bg-brand-50')}>
                      <Icon className="size-5" aria-hidden />
                      {item.badge && <CountBadge count={counts[item.badge]} className="absolute -top-1 right-0.5 ring-surface" />}
                    </span>
                    <span className="max-w-full truncate px-1">{t(item.shortLabel)}</span>
                  </>
                )}
              </NavLink>
            </li>
          )
        })}
      </ul>
    </nav>
  )
}
