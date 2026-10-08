import { useTranslation } from 'react-i18next'
import { NavLink } from 'react-router'
import { useAlerts, useNotifications } from '../api/queries'
import { useCan, useIsAdmin, usePermissions } from '../domain/access'
import { cn } from '../lib/cn'
import { Brand } from './Brand'
import { navGroups, type NavGroup, type NavItem } from './nav'

export function CountBadge({ count, className }: { count: number; className?: string }) {
  if (count <= 0) return null
  return (
    <span className={cn('flex h-5 min-w-5 items-center justify-center rounded-full bg-red-500 px-1 text-[11px] font-bold text-white ring-2 ring-navy-900', className)}>
      {count > 99 ? '99+' : count}
    </span>
  )
}

export interface BadgeCounts {
  unread: number
  alerts: number
}

/** Counts behind the navigation badges: unread notifications, unacknowledged alerts the user may see. */
export function useBadgeCounts(): BadgeCounts {
  const canView = useCan('fleet.view')
  const unread = useNotifications().data?.unread ?? 0
  const alerts = useAlerts('open', canView).data?.counts.unacked ?? 0
  return { unread, alerts }
}

/** Sections the signed-in user may open. */
export function useNav(): NavGroup[] {
  const permissions = usePermissions()
  const admin = useIsAdmin()
  const allowed = (item: NavItem) => (!item.adminOnly || admin) && (!item.permission || permissions.has(item.permission))
  return navGroups.map((g) => ({ ...g, items: g.items.filter(allowed) })).filter((g) => g.items.length > 0)
}

function SideLink({ item, counts }: { item: NavItem; counts: BadgeCounts }) {
  const { t } = useTranslation()
  const Icon = item.icon
  return (
    <NavLink
      to={item.to}
      end={item.end}
      className={({ isActive }) => cn(
        'group flex h-10 items-center gap-3 rounded-xl px-3.5 text-control font-medium transition-colors',
        isActive ? 'bg-brand-600 font-semibold text-white' : 'text-white/75 hover:bg-white/8 hover:text-white dark:text-white/85',
      )}
    >
      <span className="relative">
        <Icon className="size-5" aria-hidden />
        {item.badge && <CountBadge count={counts[item.badge]} className="absolute -top-2.5 -right-3" />}
      </span>
      <span className="truncate">{t(item.label)}</span>
    </NavLink>
  )
}

export function Sidebar() {
  const { t } = useTranslation()
  const counts = useBadgeCounts()
  const groups = useNav()
  return (
    <aside className="fixed inset-y-0 left-0 z-40 hidden w-64 flex-col border-r border-white/5 bg-eiu-deep lg:flex">
      <div className="px-5 pt-6 pb-6"><Brand /></div>
      <nav aria-label={t('nav.main')} className="scrollbar-none flex min-h-0 flex-1 flex-col gap-0.5 overflow-y-auto px-3 pb-6">
        {groups.map((group) => (
          <div key={group.title ?? 'main'} className={cn('flex flex-col gap-0.5', group.title && 'mt-5 border-t border-white/10 pt-4')}>
            {group.title && <p className="px-3.5 pb-2 text-meta font-semibold tracking-wider text-white/45 uppercase dark:text-white/55">{t(group.title)}</p>}
            {group.items.map((item) => <SideLink key={item.to} item={item} counts={counts} />)}
          </div>
        ))}
      </nav>
    </aside>
  )
}
