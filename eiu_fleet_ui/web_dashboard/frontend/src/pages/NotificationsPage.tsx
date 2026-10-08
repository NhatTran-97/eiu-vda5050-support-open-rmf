import { BellOff, CalendarDays, Check, CheckCheck, Ellipsis } from 'lucide-react'
import { useMemo } from 'react'
import { useTranslation } from 'react-i18next'
import { useSearchParams } from 'react-router'
import { useMarkNotificationsRead, useNotifications } from '../api/queries'
import type { AppNotification } from '../api/types'
import { Button, IconButton } from '../components/ui/Button'
import { Card } from '../components/ui/Card'
import { Menu, MenuItem } from '../components/ui/Menu'
import { EmptyState, ErrorState, Skeleton } from '../components/ui/States'
import { Tabs } from '../components/ui/Tabs'
import { notificationCategory, notificationStyle, notificationTarget, useNotificationText, type NotificationCategory } from '../features/notifications/meta'
import { PageHeader } from '../layout/PageHeader'
import { AlertsPanel } from '../features/operations/AlertsPanel'
import { useCan } from '../domain/access'
import { useUi } from '../lib/ui'
import { cn } from '../lib/cn'
import { useFormat } from '../lib/format'
import { useNow } from '../lib/useNow'

const DAY_MS = 24 * 3600_000
const WEEK_DAYS = 7

type Group = 'today' | 'thisWeek' | 'earlier'
type Filter = 'all' | NotificationCategory
const FILTERS: Filter[] = ['all', 'action', 'deliveries', 'issues']

function NotificationRow({ n }: { n: AppNotification }) {
  const { t } = useTranslation()
  const openTask = useUi((s) => s.openTask)
  const format = useFormat()
  const text = useNotificationText()
  const markRead = useMarkNotificationsRead()
  const { icon: Icon, tone } = notificationStyle[n.type]
  const { title, body } = text(n)
  const target = notificationTarget(n)
  const unread = n.readAt === null
  const category = notificationCategory[n.type]
  const action = category === 'action'

  const open = () => {
    if (unread) markRead.mutate(n.id)
    if (target) openTask(target.taskId)
  }

  return (
    <li className={cn('relative flex gap-3 px-4 py-3.5 sm:gap-4 sm:px-5', unread && (action ? 'bg-amber-50/70' : 'bg-brand-50/40'))}>
      {unread && <span aria-hidden className={cn('absolute inset-y-3 left-0 w-1 rounded-r', action ? 'bg-amber-500' : 'bg-brand-500')} />}
      <span className={cn('flex size-10 shrink-0 items-center justify-center rounded-full', tone)}><Icon className="size-5" /></span>
      <div className="min-w-0 flex-1">
        {action && unread && <span className="mb-1 inline-flex rounded-full bg-amber-100 px-2 py-0.5 text-xs font-semibold text-amber-800">{t('notifications.category.action')}</span>}
        <p className={cn('text-card-title text-slate-900', unread ? 'font-bold' : 'font-semibold')}>{title}</p>
        <p className="mt-0.5 text-body text-slate-600">{body}</p>
        <p className="mt-1 text-meta text-slate-500 md:hidden">{format.dateTime(n.createdAt)}</p>
        {target && (
          <Button variant="secondary" size="sm" className="mt-3 md:hidden" onClick={open}>
            {t('notifications.viewTask')}
          </Button>
        )}
      </div>
      <p className="hidden w-36 shrink-0 pt-0.5 text-right text-meta text-slate-500 md:block">{format.dateTime(n.createdAt)}</p>
      {target && (
        <div className="hidden shrink-0 md:block">
          <Button variant="secondary" size="sm" onClick={open}>
            {t('notifications.viewTask')}
          </Button>
        </div>
      )}
      <Menu trigger={({ toggle, open: isOpen }) => (
        <IconButton label={t('common.moreActions')} onClick={toggle} aria-expanded={isOpen} className="-mr-2"><Ellipsis className="size-5" /></IconButton>
      )}>
        {(close) => (
          <MenuItem onClick={() => { close(); markRead.mutate(n.id) }}><Check />{t('notifications.markRead')}</MenuItem>
        )}
      </Menu>
    </li>
  )
}

/** Notifications: alerts of the robots and services the user monitors, and updates of the user's own tasks. */
export default function NotificationsPage() {
  const { t } = useTranslation()
  const canFleet = useCan('fleet.view')
  const [params, setParams] = useSearchParams()
  const tab = canFleet && params.get('tab') !== 'tasks' ? 'alerts' : 'tasks'
  const unread = useNotifications().data?.unread
  return (
    <>
      <PageHeader title={t('pages.notifications.title')} subtitle={t('pages.notifications.subtitle')} />
      {canFleet && (
        <Tabs<'alerts' | 'tasks'> value={tab} onChange={(v) => setParams(v === 'alerts' ? {} : { tab: v }, { replace: true })} label={t('pages.notifications.title')}
          className="mb-4 rounded-2xl bg-surface p-1.5 shadow-card ring-1 ring-slate-900/5"
          items={[{ value: 'alerts', label: t('notifications.tabAlerts') }, { value: 'tasks', label: t('notifications.tabTasks'), count: unread || undefined }]} />
      )}
      {tab === 'alerts' ? <AlertsPanel /> : <TaskNotifications />}
    </>
  )
}

function TaskNotifications() {
  const { t } = useTranslation()
  const format = useFormat()
  const now = useNow(60_000)
  const list = useNotifications()
  const markAll = useMarkNotificationsRead()
  const [params, setParams] = useSearchParams()
  const filter = (FILTERS.includes(params.get('show') as Filter) ? params.get('show') : 'all') as Filter
  const keep = (next: Record<string, string>) => setParams({ ...(params.get('tab') ? { tab: params.get('tab')! } : {}), ...next }, { replace: true })
  const unreadBy = useMemo(() => {
    const out: Record<Filter, number> = { all: 0, action: 0, deliveries: 0, issues: 0 }
    for (const n of list.data?.items ?? []) {
      if (n.readAt !== null) continue
      out.all += 1
      out[notificationCategory[n.type]] += 1
    }
    return out
  }, [list.data])

  const groups = useMemo(() => {
    const today = format.dayKey(now)
    const weekStart = now - WEEK_DAYS * DAY_MS
    const out: Record<Group, AppNotification[]> = { today: [], thisWeek: [], earlier: [] }
    for (const n of (list.data?.items ?? []).filter((x) => filter === 'all' || notificationCategory[x.type] === filter)) {
      if (format.dayKey(n.createdAt) === today) out.today.push(n)
      else if (n.createdAt >= weekStart) out.thisWeek.push(n)
      else out.earlier.push(n)
    }
    return out
  }, [list.data, format, now, filter])

  const unread = list.data?.unread ?? 0
  const firstGroup = (['today', 'thisWeek', 'earlier'] as const).find((g) => groups[g].length > 0)

  return (
    <>
      <Card className="overflow-hidden">
        <div className="border-b border-slate-100 p-2 sm:px-4">
          <Tabs<Filter>
            value={filter}
            onChange={(v) => keep(v === 'all' ? {} : { show: v })}
            label={t('pages.notifications.title')}
            items={FILTERS.map((f) => ({ value: f, label: t(`notifications.category.${f}`), count: unreadBy[f] || undefined }))}
          />
        </div>
        {list.isSuccess && list.data.items.length > 0 && !firstGroup && <EmptyState icon={<BellOff />} title={t('notifications.emptyCategory')} />}
        {list.isPending && <div className="space-y-3 p-6">{[0, 1, 2, 3].map((i) => <Skeleton key={i} className="h-16" />)}</div>}
        {list.isError && <ErrorState onRetry={() => void list.refetch()} />}
        {list.isSuccess && list.data.items.length === 0 && <EmptyState icon={<BellOff />} title={t('notifications.empty')} />}
        {(['today', 'thisWeek', 'earlier'] as const).map((group) => {
          const items = groups[group]
          if (items.length === 0) return null
          const newest = items[0].createdAt
          const oldest = items[items.length - 1].createdAt
          return (
            <section key={group} aria-labelledby={`group-${group}`} className="border-b border-slate-100 last:border-0">
              <div className="flex flex-wrap items-center gap-x-2.5 gap-y-2 px-4 pt-4 pb-1.5 sm:px-5">
                <CalendarDays className="size-4 text-slate-400" aria-hidden />
                <h2 id={`group-${group}`} className="text-card-title font-semibold text-slate-900">{t(`notifications.${group}`)}</h2>
                <span className="text-meta text-slate-500">{group === 'today' ? format.date(newest) : format.dateRange(oldest, newest)}</span>
                <span className="flex-1" />
                {group === firstGroup && unread > 0 && (
                  <Button size="sm" variant="secondary" icon={<CheckCheck className="size-4" />} loading={markAll.isPending} onClick={() => markAll.mutate('all')}>
                    {t('notifications.markAllRead')}
                  </Button>
                )}
              </div>
              <ul className="divide-y divide-slate-100">
                {items.map((n) => <NotificationRow key={n.id} n={n} />)}
              </ul>
            </section>
          )
        })}
      </Card>
    </>
  )
}
