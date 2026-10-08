import {
  Bell, CalendarDays, ChartColumn, ClipboardList, LayoutDashboard, MapPinned, Menu, Plug, Radar, Settings, Truck, Users, Wrench,
  type LucideIcon,
} from 'lucide-react'
import type { Permission } from '../api/types'

export interface NavItem {
  to: string
  label: string
  shortLabel: string
  icon: LucideIcon
  end?: boolean
  badge?: 'unread' | 'alerts'
  permission?: Permission
  /** Only the admin role sees it. */
  adminOnly?: boolean
  /** Left out of the phone tab bar (reachable from the menu page). */
  desktopOnly?: boolean
}

export interface NavGroup {
  /** i18n key of the group title; null for the main items. */
  title: string | null
  items: NavItem[]
}

/** Sections of the platform. Robot types are filters inside these pages, never sections of their own. */
export const navGroups: NavGroup[] = [
  { title: null, items: [
    { to: '/overview', label: 'nav.overview', shortLabel: 'nav.overview', icon: LayoutDashboard },
    { to: '/live-operations', label: 'nav.live', shortLabel: 'nav.liveShort', icon: Radar, badge: 'alerts' },
    { to: '/tasks', label: 'nav.tasks', shortLabel: 'nav.tasks', icon: ClipboardList },
    { to: '/fleet', label: 'nav.fleet', shortLabel: 'nav.fleet', icon: Truck, permission: 'fleet.view' },
    { to: '/schedule', label: 'nav.schedule', shortLabel: 'nav.schedule', icon: CalendarDays, desktopOnly: true },
    { to: '/maintenance', label: 'nav.maintenance', shortLabel: 'nav.maintenance', icon: Wrench, permission: 'maintenance.view', desktopOnly: true },
    { to: '/analytics', label: 'nav.analytics', shortLabel: 'nav.analytics', icon: ChartColumn, permission: 'analytics.view', desktopOnly: true },
    { to: '/notifications', label: 'nav.notifications', shortLabel: 'nav.notificationsShort', icon: Bell, badge: 'unread' },
  ] },
  { title: 'nav.administration', items: [
    { to: '/admin/users', label: 'nav.users', shortLabel: 'nav.users', icon: Users, adminOnly: true, desktopOnly: true },
    { to: '/admin/locations', label: 'nav.locations', shortLabel: 'nav.locations', icon: MapPinned, adminOnly: true, desktopOnly: true },
    { to: '/admin/integrations', label: 'nav.integrations', shortLabel: 'nav.integrations', icon: Plug, adminOnly: true, desktopOnly: true },
    { to: '/admin/settings', label: 'nav.systemSettings', shortLabel: 'nav.systemSettings', icon: Settings, adminOnly: true, desktopOnly: true },
  ] },
]

/** Last tab of the phone tab bar: every section as a list. */
export const menuItem: NavItem = { to: '/menu', label: 'nav.menu', shortLabel: 'nav.menu', icon: Menu }
