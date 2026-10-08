import { Bell, ChevronDown, LogOut, Search, Settings, X } from 'lucide-react'
import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import { Link, useNavigate } from 'react-router'
import { useLogout, useMarkNotificationsRead, useMe, useNotifications } from '../api/queries'
import { IconButton } from '../components/ui/Button'
import { Menu, MenuItem, MenuLink, MenuSeparator } from '../components/ui/Menu'
import { LanguageSwitch } from '../features/auth/LanguageSwitch'
import { ThemeMenu } from '../features/auth/ThemeSwitch'
import { notificationStyle, notificationTarget, useNotificationText } from '../features/notifications/meta'
import { CreateTaskButton, createTaskInTopBar } from '../features/tasks/CreateTaskDialog'
import { cn } from '../lib/cn'
import { useFormat } from '../lib/format'
import { initials } from '../lib/text'
import { useUi } from '../lib/ui'
import { useLive } from '../realtime/store'
import { Brand } from './Brand'
import { GlobalSearch } from './GlobalSearch'
import { CountBadge } from './Sidebar'

const BELL_PREVIEW = 5

/** Overall system state for everyone: the live stream and whether the robot fleets can be reached. */
function SystemHealth({ light }: { light?: boolean }) {
  const { t } = useTranslation()
  const status = useLive((s) => s.status)
  const rmf = useLive((s) => s.rmf)
  const ok = status === 'open' && (rmf === 'online' || rmf === null)
  const tone = ok ? 'bg-emerald-400' : status === 'connecting' ? 'bg-amber-400' : 'bg-red-400'
  const label = status !== 'open' ? t(`topbar.live.${status}`) : ok ? t('system.normal') : t('system.fleetUnavailable')
  const degraded = ok ? 'border-ops-border bg-ops-card text-ops-muted' : status === 'connecting' ? 'border-amber-200 bg-amber-50 text-amber-700' : 'border-red-200 bg-red-50 text-red-700'
  return (
    <span className={cn('hidden items-center gap-2 rounded-xl px-3 py-2 text-xs font-medium xl:flex', light ? cn('border', degraded) : 'bg-white/6 text-white/80')} role="status">
      <span className={cn('size-2 rounded-full', tone)} aria-hidden />{label}
    </span>
  )
}

function NotificationBell({ light }: { light?: boolean }) {
  const { t } = useTranslation()
  const list = useNotifications().data
  const markRead = useMarkNotificationsRead()
  const text = useNotificationText()
  const format = useFormat()
  const unread = list?.unread ?? 0
  const openTask = useUi((st) => st.openTask)

  return (
    <>
      <Link to="/notifications" aria-label={t('topbar.notifications')} className={cn('relative flex size-10 items-center justify-center rounded-xl lg:hidden', light ? 'text-ops-muted hover:bg-ops-subtle' : 'text-white/80 hover:bg-white/10 hover:text-white')}>
        <Bell className="size-5.5" />
        <CountBadge count={unread} className="absolute top-0.5 right-0.5" />
      </Link>
      <div className="hidden lg:block">
        <Menu
          className="w-96 py-0"
          trigger={({ toggle, open }) => (
            <IconButton tone={light ? 'light' : 'dark'} label={t('topbar.notifications')} onClick={toggle} aria-expanded={open} className="relative">
              <Bell className="size-5.5" />
              <CountBadge count={unread} className="absolute top-0.5 right-0.5" />
            </IconButton>
          )}
        >
          {(close) => (
            <div>
              <div className="flex items-center justify-between border-b border-slate-100 px-4 py-3">
                <p className="font-bold text-slate-900">{t('topbar.notifications')}</p>
                {unread > 0 && (
                  <button type="button" className="text-xs font-semibold text-brand-600 hover:underline" onClick={() => markRead.mutate('all')}>
                    {t('topbar.markAllRead')}
                  </button>
                )}
              </div>
              {!list?.items.length && <p className="px-4 py-6 text-center text-sm text-slate-500">{t('topbar.noNotifications')}</p>}
              <ul className="divide-y divide-slate-100">
                {list?.items.slice(0, BELL_PREVIEW).map((n) => {
                  const { icon: Icon, tone } = notificationStyle[n.type]
                  const { title, body } = text(n)
                  const target = notificationTarget(n)
                  return (
                    <li key={n.id}>
                      <button
                        type="button"
                        className={cn('flex w-full gap-3 px-4 py-3 text-left hover:bg-slate-50', n.readAt === null && 'bg-brand-50/50')}
                        onClick={() => {
                          if (n.readAt === null) markRead.mutate(n.id)
                          close()
                          if (target) openTask(target.taskId)
                        }}
                      >
                        <span className={cn('flex size-9 shrink-0 items-center justify-center rounded-full', tone)}><Icon className="size-4.5" /></span>
                        <span className="min-w-0 flex-1">
                          <span className="block text-sm font-semibold text-slate-900">{title}</span>
                          <span className="line-clamp-2 block text-xs text-slate-500">{body}</span>
                          <span className="mt-0.5 block text-xs text-slate-400">{format.dateTime(n.createdAt)}</span>
                        </span>
                        {n.readAt === null && <span className="mt-1.5 size-2 shrink-0 rounded-full bg-brand-500" />}
                      </button>
                    </li>
                  )
                })}
              </ul>
              <Link to="/notifications" onClick={close} className="block border-t border-slate-100 px-4 py-3 text-center text-sm font-semibold text-brand-600 hover:bg-slate-50">
                {t('topbar.viewAll')}
              </Link>
            </div>
          )}
        </Menu>
      </div>
    </>
  )
}

function UserMenu({ light }: { light?: boolean }) {
  const { t } = useTranslation()
  const navigate = useNavigate()
  const me = useMe().data
  const logout = useLogout()
  if (!me) return null

  return (
    <Menu
      className="w-72"
      trigger={({ toggle, open }) => (
        <button type="button" onClick={toggle} aria-expanded={open} aria-label={t('topbar.userMenu')}
          className={cn('flex h-11 items-center gap-2.5 rounded-xl pr-2 pl-1', light ? 'text-ops-text hover:bg-ops-subtle' : 'text-white hover:bg-white/10')}>
          <span className={cn('flex size-9 items-center justify-center rounded-full text-sm font-bold', light ? 'bg-eiu-navy text-white' : 'bg-white text-navy-900')}>{initials(me.fullName)}</span>
          <span className="hidden text-left leading-tight md:block">
            <span className="block max-w-36 truncate text-sm font-semibold">{me.fullName}</span>
            <span className={cn('block text-xs', light ? 'text-ops-muted' : 'text-white/60')}>{t(`role.${me.role}`)}</span>
          </span>
          <ChevronDown className={cn('hidden size-4 md:block', light ? 'text-ops-muted' : 'text-white/70')} />
        </button>
      )}
    >
      {(close) => (
        <>
          <div className="px-4 pt-2 pb-3">
            <p className="truncate font-semibold text-slate-900">{me.fullName}</p>
            <p className="truncate text-xs text-slate-500">{me.email}</p>
            <span className="mt-2 inline-flex rounded-full bg-brand-50 px-2 py-0.5 text-xs font-semibold text-brand-700">{t(`role.${me.role}`)}</span>
          </div>
          {/* The one place the dashboard names the university; the product identity stays primary everywhere else. On phones
              only the university line remains. Each line truncates instead of wrapping. */}
          <p className="mx-4 mb-3 border-t border-slate-100 pt-2.5 text-meta leading-snug text-slate-500">
            <span className="hidden truncate font-medium text-slate-600 sm:block">{t('app.brand')}</span>
            <span className="block truncate">{t('app.institution')}</span>
          </p>
          <div className="flex items-center justify-between gap-3 px-4 pb-3">
            <span className="text-xs font-semibold text-slate-500">{t('topbar.language')}</span>
            <LanguageSwitch />
          </div>
          <MenuSeparator />
          <MenuLink to="/account" onClick={close}><Settings />{t('nav.account')}</MenuLink>
          <MenuSeparator />
          <MenuItem danger onClick={() => {
            close()
            logout.mutate(undefined, { onSettled: () => navigate('/login', { replace: true }) })
          }}>
            <LogOut />{t('topbar.signOut')}
          </MenuItem>
        </>
      )}
    </Menu>
  )
}

export function TopBar({ light }: { light?: boolean }) {
  const { t } = useTranslation()
  const [searchOpen, setSearchOpen] = useState(false)
  const tone = light ? 'light' : 'dark'

  return (
    <header className={cn('sticky top-0 z-30 border-b backdrop-blur-md', light ? 'border-ops-border bg-ops-card/90' : 'border-white/5 bg-navy-900/85')}>
      <div className="relative mx-auto flex h-16 max-w-[2560px] items-center gap-2 px-4 sm:px-6 lg:h-[72px] lg:gap-3 lg:px-8">
        <Brand compact light={light} className="min-w-0 lg:hidden" />
        <GlobalSearch light={light} className="hidden max-w-xl flex-1 md:block" />
        <div className="flex-1" />
        <IconButton tone={tone} label={t('topbar.search')} className="md:hidden" onClick={() => setSearchOpen(true)}>
          <Search className="size-5.5" />
        </IconButton>
        <SystemHealth light={light} />
        <span className={createTaskInTopBar}><CreateTaskButton /></span>
        <div className="hidden xl:block"><LanguageSwitch tone={tone} /></div>
        <ThemeMenu tone={tone} />
        <NotificationBell light={light} />
        <UserMenu light={light} />

        {searchOpen && (
          <div className={cn('absolute inset-0 flex items-center gap-2 px-4 md:hidden', light ? 'bg-ops-card' : 'bg-navy-900')}>
            <GlobalSearch light={light} autoFocus onDone={() => setSearchOpen(false)} className="flex-1" />
            <IconButton tone={tone} label={t('search.close')} onClick={() => setSearchOpen(false)}>
              <X className="size-5.5" />
            </IconButton>
          </div>
        )}
      </div>
    </header>
  )
}
