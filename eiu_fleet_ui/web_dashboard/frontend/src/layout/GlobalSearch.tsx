import { Bot, ClipboardList, MapPin, Search, Shapes, UserRound } from 'lucide-react'
import { useEffect, useId, useMemo, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { useNavigate } from 'react-router'
import { useSearch } from '../api/queries'
import { useServiceName } from '../domain/services'
import { taskCode } from '../features/tasks/taskText'
import { cn } from '../lib/cn'
import { useLocalize } from '../lib/i18nText'
import { useUi } from '../lib/ui'

interface Result {
  key: string
  label: string
  detail: string
  icon: typeof Bot
  /** Page to open, or a drawer. */
  to?: string
  robot?: string
  task?: number
  /** i18n key of the result group under `search.` */
  group: string
}

/** Robots, tasks, locations, zones and (admins) operators matching the query, as the server allows this user. */
function useResults(query: string): Result[] {
  const { t } = useTranslation()
  const localize = useLocalize()
  const serviceName = useServiceName()
  const [debounced, setDebounced] = useState(query)
  useEffect(() => {
    const id = window.setTimeout(() => setDebounced(query.trim()), 200)
    return () => window.clearTimeout(id)
  }, [query])
  const data = useSearch(debounced).data
  return useMemo<Result[]>(() => {
    if (!data || !query.trim()) return []
    return [
      ...data.robots.map<Result>((r) => ({ key: `r-${r.name}`, label: r.name, detail: `${serviceName(r.serviceType)} · ${t(`robotStatus.${r.status}`)}`,
        icon: Bot, robot: r.name, group: 'robots' })),
      ...data.tasks.map<Result>((x) => ({ key: `t-${x.id}`, label: taskCode(x.id, x.service),
        detail: `${serviceName(x.service)} · ${x.area ? localize(x.area.name) : x.route ? localize(x.route.name) : `${localize(x.pickup.name)} → ${localize(x.dropoff.name)}`}`,
        icon: ClipboardList, task: x.id, group: 'tasks' })),
      ...data.locations.map<Result>((l) => ({ key: `l-${l.id}`, label: localize(l.name), detail: localize(l.building), icon: MapPin,
        to: '/live-operations', group: 'locations' })),
      ...data.zones.map<Result>((z) => ({ key: `z-${z.id}`, label: localize(z.name), detail: z.id, icon: Shapes, to: `/schedule?zone=${encodeURIComponent(z.id)}`, group: 'zones' })),
      ...data.users.map<Result>((u) => ({ key: `u-${u.id}`, label: u.fullName, detail: `${u.email} · ${t(`role.${u.role}`)}`, icon: UserRound,
        to: `/admin/users?q=${encodeURIComponent(u.email)}`, group: 'users' })),
    ]
  }, [data, query, t, localize, serviceName])
}

interface SearchProps {
  light?: boolean
  autoFocus?: boolean
  onDone?: () => void
  className?: string
}

/** Search box of the top bar. */
export function GlobalSearch(props: SearchProps) {
  return <SearchBox {...props} />
}

function SearchBox({ autoFocus, onDone, className, light }: SearchProps) {
  const { t } = useTranslation()
  const navigate = useNavigate()
  const openRobot = useUi((s) => s.openRobot)
  const openTask = useUi((s) => s.openTask)
  const placeholder = 'topbar.searchPlaceholder'
  const listId = useId()
  const [query, setQuery] = useState('')
  const [open, setOpen] = useState(false)
  const [active, setActive] = useState(0)
  const results = useResults(query)
  const groups = [...new Set(results.map((r) => r.group))]

  const select = (result: Result | undefined) => {
    if (!result) return
    if (result.robot) openRobot(result.robot)
    else if (result.task !== undefined) openTask(result.task)
    else if (result.to) navigate(result.to)
    setQuery('')
    setOpen(false)
    onDone?.()
  }

  const showList = open && query.trim().length > 0

  return (
    <div className={cn('relative', className)}>
      <Search className={cn('pointer-events-none absolute top-1/2 left-4 size-5 -translate-y-1/2', light ? 'text-ops-muted' : 'text-white/60')} aria-hidden />
      <input
        type="search"
        role="combobox"
        aria-expanded={showList}
        aria-controls={listId}
        aria-activedescendant={showList && results[active] ? `${listId}-${results[active].key}` : undefined}
        aria-label={t('topbar.search')}
        placeholder={t(placeholder)}
        autoFocus={autoFocus}
        value={query}
        onChange={(e) => {
          setQuery(e.target.value)
          setActive(0)
          setOpen(true)
        }}
        onFocus={() => setOpen(true)}
        onBlur={() => window.setTimeout(() => setOpen(false), 150)}
        onKeyDown={(e) => {
          if (e.key === 'ArrowDown') {
            e.preventDefault()
            setActive((i) => Math.min(i + 1, results.length - 1))
          } else if (e.key === 'ArrowUp') {
            e.preventDefault()
            setActive((i) => Math.max(i - 1, 0))
          } else if (e.key === 'Enter') {
            e.preventDefault()
            select(results[active])
          } else if (e.key === 'Escape') {
            setOpen(false)
            onDone?.()
          }
        }}
        className={cn('h-11 w-full rounded-xl border pr-4 pl-12 text-[15px] focus:outline-none', light
          ? 'border-ops-border bg-ops-bg text-ops-text placeholder:text-ops-muted focus:border-ops-blue focus:bg-ops-card'
          : 'border-white/15 bg-white/8 text-white placeholder:text-white/50 focus:border-brand-400 focus:bg-white/12')}
      />
      {showList && (
        <div id={listId} role="listbox" className="absolute inset-x-0 top-full z-50 mt-2 max-h-[60dvh] overflow-y-auto rounded-xl bg-surface py-2 shadow-xl ring-1 ring-slate-900/10">
          {results.length === 0 && <p className="px-4 py-3 text-sm text-slate-500">{t('search.noResults', { query })}</p>}
          {groups.map((group) => {
            const items = results.filter((r) => r.group === group)
            if (items.length === 0) return null
            return (
              <div key={group} role="group" aria-label={t(`search.${group}`)}>
                <p className="px-4 pt-2 pb-1 text-xs font-semibold tracking-wide text-slate-400 uppercase">{t(`search.${group}`)}</p>
                {items.map((r) => {
                  const index = results.indexOf(r)
                  const Icon = r.icon
                  return (
                    <div
                      key={r.key}
                      id={`${listId}-${r.key}`}
                      role="option"
                      aria-selected={index === active}
                      onMouseDown={(e) => e.preventDefault()}
                      onClick={() => select(r)}
                      onMouseEnter={() => setActive(index)}
                      className={cn('flex cursor-pointer items-center gap-3 px-4 py-2.5 text-sm', index === active && 'bg-brand-50')}
                    >
                      <Icon className="size-4 shrink-0 text-slate-400" aria-hidden />
                      <span className="font-semibold text-slate-900">{r.label}</span>
                      {r.detail && <span className="truncate text-slate-500">{r.detail}</span>}
                    </div>
                  )
                })}
              </div>
            )
          })}
        </div>
      )}
    </div>
  )
}
