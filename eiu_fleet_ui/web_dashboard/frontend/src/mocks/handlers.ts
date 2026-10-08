// REST endpoints of the demo backend, served in the browser by MSW.
import { delay, http, HttpResponse } from 'msw'
import { API_BASE } from '../app/env'
import type {
  DeliveryGroup,
  DeliveryList,
  Locale,
  MaintenanceItem,
  NewTask,
  NotificationPrefs,
  Permission,
  SavedLocation,
  SiteConfig,
} from '../api/types'
import { can, deny, Forbidden, servicesOf } from './access'
import { db, resetDb, save } from './db'
import type { UserRow } from './schema'
import { demoSettings } from './settings'
import * as ops from './operations'
import * as platform from './platform'
import { markChanged, tick } from './simulator'
import { levelGraph } from './site'
import { toDelivery, toLevels, toLocation, toMe, toNotification } from './views'

const url = (path: string) => API_BASE + path
const LOCALES: Locale[] = ['vi', 'en']
const LOGIN_LOCK_FAILURES = 5
const LOGIN_LOCK_MS = 60_000

const loginFailures = new Map<string, { count: number; until: number }>()

function fail(status: number, code: string, message = '') {
  return HttpResponse.json({ error: message ? { code, message } : { code } }, { status })
}

class HttpFailure extends Error {
  constructor(readonly status: number, readonly code: string) {
    super(code)
  }
}

function currentUser(): UserRow {
  const data = db()
  const user = data.session && data.users.find((u) => u.id === data.session!.userId && u.active)
  if (!user) throw new HttpFailure(401, 'auth.required')
  return user
}

function requirePermission(permission: Permission): UserRow {
  const user = currentUser()
  if (!can(user, permission)) throw deny(user, 403, permission.startsWith('task.') || permission.startsWith('fleet.') || permission.startsWith('alerts.')
    || permission === 'analytics.view' || permission === 'maintenance.view' ? 'PERMISSION_DENIED' : 'ADMIN_ONLY')
  return user
}

function checkCsrf(request: Request): void {
  const token = request.headers.get('X-CSRF-Token')
  if (!token || token !== db().session?.csrf) throw new HttpFailure(403, 'auth.csrf')
}

function setCsrfCookie(value: string | null): void {
  document.cookie = value
    ? `csrf_token=${encodeURIComponent(value)}; path=/; SameSite=Strict`
    : 'csrf_token=; path=/; max-age=0; SameSite=Strict'
}

type Handler = (args: { request: Request; params: Record<string, string | readonly string[] | undefined> }) =>
  Promise<Response> | Response

/** Simulated latency, a catch-up tick and error mapping around every endpoint. */
function endpoint(handler: Handler, options: { mutates?: boolean } = {}): Handler {
  return async (args) => {
    await delay(demoSettings.latencyMs)
    tick(Date.now())
    ops.tickAlerts(Date.now())
    try {
      if (options.mutates) checkCsrf(args.request)
      return await handler(args)
    } catch (e) {
      if (e instanceof Forbidden) return fail(e.status, e.code, e.detail)
      if (e instanceof HttpFailure || e instanceof ops.OperationError) return fail(e.status, e.code)
      throw e
    }
  }
}

export const handlers = [
  http.get(url('/config'), endpoint(() => {
    const config: SiteConfig = {
      siteName: { vi: 'EIU Robot Services', en: 'EIU Robot Services' },
      timeZone: demoSettings.timeZone,
      defaultLocale: 'vi',
      locales: LOCALES,
      maxActiveDeliveries: demoSettings.maxActiveDeliveries,
      maxPatrolStops: demoSettings.maxPatrolStops,
      maxPatrolRounds: demoSettings.maxPatrolRounds,
      minPasswordLength: demoSettings.minPasswordLength,
      support: demoSettings.support,
      demo: true,
      demoAccounts: db().users.filter((u) => u.active).map((u) => ({ role: u.role, email: u.email, password: u.password, name: u.fullName })),
    }
    return HttpResponse.json(config)
  })),

  // Session

  http.post(url('/auth/login'), endpoint(async ({ request }) => {
    const body = (await request.json()) as { email?: string; password?: string }
    const email = String(body.email ?? '').trim().toLowerCase()
    const now = Date.now()
    const lock = loginFailures.get(email)
    if (lock && lock.until > now) return fail(429, 'auth.too_many_attempts')

    const data = db()
    const user = data.users.find((u) => u.email.toLowerCase() === email && u.active)
    if (!user || user.password !== body.password) {
      const count = (lock?.count ?? 0) + 1
      loginFailures.set(email, { count, until: count >= LOGIN_LOCK_FAILURES ? now + LOGIN_LOCK_MS : 0 })
      return fail(401, 'auth.invalid_credentials')
    }
    loginFailures.delete(email)
    const csrf = crypto.randomUUID()
    data.session = { userId: user.id, csrf }
    user.lastLoginAt = Date.now()
    save()
    setCsrfCookie(csrf)
    return HttpResponse.json(toMe(user))
  })),

  http.post(url('/auth/logout'), endpoint(() => {
    db().session = null
    save()
    setCsrfCookie(null)
    return new HttpResponse(null, { status: 204 })
  })),

  http.get(url('/auth/me'), endpoint(() => HttpResponse.json(toMe(currentUser())))),

  http.put(url('/me/preferences'), endpoint(async ({ request }) => {
    const user = currentUser()
    const body = (await request.json()) as { locale?: Locale; notificationPrefs?: Partial<NotificationPrefs> }
    if (body.locale !== undefined) {
      if (!LOCALES.includes(body.locale)) return fail(422, 'prefs.invalid_locale')
      user.locale = body.locale
    }
    if (body.notificationPrefs) {
      const p = body.notificationPrefs
      if (typeof p.deliveryUpdates === 'boolean') user.notificationPrefs.deliveryUpdates = p.deliveryUpdates
      if (typeof p.delays === 'boolean') user.notificationPrefs.delays = p.delays
    }
    save()
    return HttpResponse.json(toMe(user))
  }, { mutates: true })),

  http.post(url('/me/password'), endpoint(async ({ request }) => {
    const user = currentUser()
    const body = (await request.json()) as { currentPassword?: string; newPassword?: string }
    if (body.currentPassword !== user.password) return fail(422, 'auth.wrong_password')
    if (String(body.newPassword ?? '').length < demoSettings.minPasswordLength) return fail(422, 'auth.password_too_short')
    user.password = String(body.newPassword)
    save()
    return new HttpResponse(null, { status: 204 })
  }, { mutates: true })),

  // Site data

  http.get(url('/levels'), endpoint(() => {
    currentUser()
    return HttpResponse.json(toLevels(db()))
  })),

  http.get(url('/levels/:id/graph'), endpoint(({ params }) => {
    currentUser()
    const graph = levelGraph(String(params.id))
    return graph ? HttpResponse.json(graph) : fail(404, 'level.not_found')
  })),

  http.get(url('/locations'), endpoint(() => {
    currentUser()
    return HttpResponse.json(db().locations.map(toLocation))
  })),

  http.get(url('/me/saved-locations'), endpoint(() => {
    const user = currentUser()
    return HttpResponse.json(db().saved[user.id] ?? [])
  })),

  http.put(url('/me/saved-locations'), endpoint(async ({ request }) => {
    const user = currentUser()
    const body = (await request.json()) as SavedLocation[]
    const data = db()
    if (!Array.isArray(body)) return fail(422, 'saved.invalid')
    const seen = new Set<string>()
    const items: SavedLocation[] = []
    for (const item of body) {
      if (!data.locations.some((l) => l.id === item?.locationId)) return fail(422, 'saved.unknown_location')
      if (seen.has(item.locationId)) continue
      seen.add(item.locationId)
      items.push({ locationId: item.locationId, starred: item.starred === true })
    }
    data.saved[user.id] = items
    save()
    return HttpResponse.json(items)
  }, { mutates: true })),

  http.get(url('/me/recent-destinations'), endpoint(() => {
    const user = currentUser()
    const seen = new Set<string>()
    const recent = db().deliveries
      .filter((d) => d.requesterId === user.id && d.status !== 'scheduled')
      .sort((a, b) => b.createdAt - a.createdAt)
      .filter((d) => (seen.has(d.dropoffId) ? false : (seen.add(d.dropoffId), true)))
      .slice(0, 4)
      .map((d) => ({ locationId: d.dropoffId, lastUsedAt: d.createdAt }))
    return HttpResponse.json(recent)
  })),

  http.get(url('/templates'), endpoint(() => {
    const allowed = servicesOf(currentUser())
    return HttpResponse.json(db().templates.filter((tpl) => allowed.includes(tpl.service)))
  })),

  // Tasks of the user (first release)

  http.get(url('/deliveries'), endpoint(({ request }) => {
    const user = currentUser()
    const group = (new URL(request.url).searchParams.get('group') ?? 'all') as DeliveryGroup | 'all'
    const data = db()
    const own = data.deliveries.filter((d) => d.requesterId === user.id)
    const rank: Record<DeliveryGroup, number> = { active: 0, upcoming: 1, completed: 2 }
    const sorted = own
      .map((row) => toDelivery(data, row, user))
      .sort((a, b) => rank[a.group] - rank[b.group]
        || (a.group === 'upcoming' ? (a.scheduledAt ?? 0) - (b.scheduledAt ?? 0) : b.createdAt - a.createdAt))
    const count = (g: DeliveryGroup) => sorted.filter((d) => d.group === g).length
    const list: DeliveryList = {
      items: group === 'all' ? sorted : sorted.filter((d) => d.group === group),
      counts: { all: sorted.length, active: count('active'), upcoming: count('upcoming'), completed: count('completed') },
      nextCursor: null,
    }
    return HttpResponse.json(list)
  })),

  // Notifications

  http.get(url('/notifications'), endpoint(() => {
    const user = currentUser()
    const own = db().notifications.filter((n) => n.userId === user.id).sort((a, b) => b.createdAt - a.createdAt)
    return HttpResponse.json({
      items: own.map(toNotification),
      unread: own.filter((n) => n.readAt === null).length,
      nextCursor: null,
    })
  })),

  http.post(url('/notifications/read-all'), endpoint(() => {
    const user = currentUser()
    const now = Date.now()
    db().notifications.forEach((n) => {
      if (n.userId === user.id && n.readAt === null) n.readAt = now
    })
    markChanged('notifications')
    save()
    return new HttpResponse(null, { status: 204 })
  }, { mutates: true })),

  http.post(url('/notifications/:id/read'), endpoint(({ params }) => {
    const user = currentUser()
    const row = db().notifications.find((n) => n.id === Number(params.id) && n.userId === user.id)
    if (!row) return fail(404, 'notification.not_found')
    row.readAt ??= Date.now()
    markChanged('notifications')
    save()
    return HttpResponse.json(toNotification(row))
  }, { mutates: true })),

  // Services, overview, tasks, schedule

  http.get(url('/services'), endpoint(() => HttpResponse.json(platform.services(currentUser())))),
  http.get(url('/catalog'), endpoint(() => HttpResponse.json(platform.catalog(currentUser())))),
  http.get(url('/overview'), endpoint(() => HttpResponse.json(platform.overview(currentUser())))),
  http.get(url('/search'), endpoint(({ request }) => HttpResponse.json(platform.search(currentUser(), new URL(request.url).searchParams.get('q') ?? '')))),

  http.get(url('/tasks'), endpoint(({ request }) => {
    const p = new URL(request.url).searchParams
    return HttpResponse.json(ops.tasks(currentUser(), p.get('group') || 'all', p.get('service_type') ?? '', p.get('q') ?? '', p.get('state') ?? '', p.get('mine') === 'true'))
  })),

  http.post(url('/tasks'), endpoint(async ({ request }) => {
    const user = currentUser()
    const row = platform.createTask(user, (await request.json()) as Partial<NewTask>)
    return HttpResponse.json(platform.taskDetail(user, row.id), { status: 201 })
  }, { mutates: true })),

  http.get(url('/tasks/:id'), endpoint(({ params }) => HttpResponse.json(platform.taskDetail(currentUser(), Number(params.id))))),

  http.post(url('/tasks/:id/:action'), endpoint(async ({ request, params }) => {
    const body = (await request.json().catch(() => ({}))) as { robot?: string }
    return HttpResponse.json(platform.taskAction(currentUser(), Number(params.id), String(params.action), body.robot))
  }, { mutates: true })),

  http.get(url('/schedule'), endpoint(({ request }) => {
    const p = new URL(request.url).searchParams
    return HttpResponse.json(platform.schedule(currentUser(), Number(p.get('start')), Number(p.get('end')),
      { service: p.get('service_type') ?? '', robot: p.get('robot') ?? '', zone: p.get('zone') ?? '' }))
  })),

  http.get(url('/analytics/activity'), endpoint(({ request }) => {
    const p = new URL(request.url).searchParams
    return HttpResponse.json(platform.activity(currentUser(), p.get('range') ?? 'today', p.get('service_type') ?? ''))
  })),

  http.get(url('/analytics'), endpoint(({ request }) => {
    const user = requirePermission('analytics.view')
    const p = new URL(request.url).searchParams
    const service = p.get('service_type') ?? ''
    if (service && !servicesOf(user).includes(service)) throw deny(user, 403, 'SERVICE_NOT_ALLOWED', { service })
    return HttpResponse.json(ops.analytics(Number(p.get('days') ?? 7), user, service, p.get('robot') ?? '', p.get('zone') ?? '', servicesOf(user)))
  })),

  http.get(url('/maintenance'), endpoint(() => HttpResponse.json(platform.maintenanceView(requirePermission('maintenance.view'))))),
  http.post(url('/maintenance'), endpoint(async ({ request }) =>
    HttpResponse.json(platform.saveMaintenance(currentUser(), null, (await request.json()) as Partial<MaintenanceItem>), { status: 201 }), { mutates: true })),
  http.patch(url('/maintenance/:id'), endpoint(async ({ request, params }) =>
    HttpResponse.json(platform.saveMaintenance(currentUser(), Number(params.id), (await request.json()) as Partial<MaintenanceItem>)), { mutates: true })),

  // Alerts

  http.get(url('/alerts'), endpoint(({ request }) => {
    const user = requirePermission('fleet.view')
    return HttpResponse.json(platform.visibleAlerts(user, new URL(request.url).searchParams.get('state') ?? 'open'))
  })),

  http.post(url('/fleet/alerts/ack-all'), endpoint(() => {
    return HttpResponse.json({ acknowledged: ops.acknowledge(requirePermission('alerts.ack'), null) })
  }, { mutates: true })),

  http.post(url('/fleet/alerts/:id/ack'), endpoint(({ params }) => {
    return HttpResponse.json({ acknowledged: ops.acknowledge(requirePermission('alerts.ack'), Number(params.id)) })
  }, { mutates: true })),

  http.post(url('/fleet/alerts/:id/resolve'), endpoint(({ params }) => {
    ops.resolveAlert(requirePermission('alerts.ack'), Number(params.id))
    return new HttpResponse(null, { status: 204 })
  }, { mutates: true })),

  // Fleet

  http.get(url('/fleet/robots'), endpoint(() => HttpResponse.json(ops.fleetRobots(Date.now(), requirePermission('fleet.view'))))),

  http.get(url('/fleet/robots/:name'), endpoint(({ request, params }) => {
    const user = requirePermission('fleet.view')
    if (!ops.fleetRobots(Date.now(), user).some((r) => r.name === params.name)) return fail(404, 'robot.not_found')
    return HttpResponse.json(ops.robotDetail(String(params.name), Number(new URL(request.url).searchParams.get('hours') ?? 24)))
  })),

  http.post(url('/fleet/robots/:robot/:action'), endpoint(async ({ request, params }) => {
    const user = requirePermission('fleet.control')
    const body = (await request.json().catch(() => ({}))) as Record<string, unknown>
    const result = ops.robotCommand(String(params.robot), String(params.action), body)
    ops.audit(user, `robot.${String(params.action).replace('-', '_')}`, String(params.robot), body)
    return HttpResponse.json(result)
  }, { mutates: true })),

  http.get(url('/fleet/registration'), endpoint(() => {
    requirePermission('robots.manage')
    return HttpResponse.json(ops.registration())
  })),

  http.post(url('/fleet/registration'), endpoint(async ({ request }) => {
    const user = requirePermission('robots.manage')
    const body = (await request.json()) as Record<string, unknown>
    const result = ops.register(body)
    if (body.action !== 'check') ops.audit(user, `registration.${String(body.action)}`, `${String(body.fleet)}/${String(body.name)}`, { ok: result.ok })
    return HttpResponse.json(result)
  }, { mutates: true })),

  http.get(url('/fleet/lanes'), endpoint(() => {
    requirePermission('fleet.view')
    return HttpResponse.json(ops.lanes())
  })),

  http.post(url('/fleet/lanes'), endpoint(async ({ request }) => {
    const user = requirePermission('locations.manage')
    const body = (await request.json()) as Record<string, unknown>
    const result = ops.setLanes(body)
    ops.audit(user, 'lanes.request', result.fleets.join(','), { close: body.close ?? [], open: body.open ?? [] })
    return HttpResponse.json(result)
  }, { mutates: true })),

  http.get(url('/fleet/nav-graph'), endpoint(({ request }) => {
    requirePermission('locations.manage')
    return HttpResponse.json(ops.navGraph(new URL(request.url).searchParams.get('level')))
  })),

  http.put(url('/fleet/nav-graph'), endpoint(async ({ request }) => {
    const user = requirePermission('locations.manage')
    const body = (await request.json()) as Parameters<typeof ops.saveNavGraph>[0]
    const result = ops.saveNavGraph(body)
    ops.audit(user, 'nav_graph.save', 'demo/nav_graph.yaml', { level: body.levelId, vertices: body.vertices?.length, lanes: body.lanes?.length })
    return HttpResponse.json(result)
  }, { mutates: true })),

  http.get(url('/fleet/system'), endpoint(() => {
    requirePermission('system.diagnostics')
    return HttpResponse.json(ops.system(Date.now()))
  })),

  // Administration

  http.get(url('/admin/overview'), endpoint(() => {
    requirePermission('system.diagnostics')
    return HttpResponse.json(ops.adminOverview(Date.now()))
  })),
  http.get(url('/admin/infrastructure'), endpoint(() => {
    requirePermission('locations.manage')
    return HttpResponse.json(ops.infrastructure())
  })),
  http.get(url('/admin/integrations'), endpoint(() => {
    requirePermission('integrations.manage')
    return HttpResponse.json(platform.integrations())
  })),
  http.get(url('/admin/settings'), endpoint(() => {
    requirePermission('settings.manage')
    return HttpResponse.json(platform.systemSettings())
  })),
  http.put(url('/admin/toggles/:kind/:id'), endpoint(async ({ request, params }) => {
    const body = (await request.json()) as { enabled?: unknown }
    return HttpResponse.json(platform.setToggle(currentUser(), String(params.kind), String(params.id), body.enabled))
  }, { mutates: true })),
  http.get(url('/admin/audit'), endpoint(({ request }) => {
    requirePermission('system.diagnostics')
    const p = new URL(request.url).searchParams
    return HttpResponse.json(ops.auditList(p.get('q') ?? '', p.get('action') ?? ''))
  })),
  http.get(url('/admin/access-catalog'), endpoint(() => {
    requirePermission('users.manage')
    return HttpResponse.json(platform.accessCatalog())
  })),
  http.get(url('/admin/users'), endpoint(() => {
    requirePermission('users.manage')
    return HttpResponse.json(db().users.map(ops.adminUser))
  })),
  http.post(url('/admin/users'), endpoint(async ({ request }) => {
    const user = requirePermission('users.manage')
    return HttpResponse.json(ops.createUser(user, (await request.json()) as Record<string, unknown>, demoSettings.minPasswordLength), { status: 201 })
  }, { mutates: true })),
  http.patch(url('/admin/users/:id'), endpoint(async ({ request, params }) => {
    const user = requirePermission('users.manage')
    const result = ops.updateUser(user, String(params.id), (await request.json()) as Record<string, unknown>)
    markChanged('access')
    return HttpResponse.json(result)
  }, { mutates: true })),
  http.post(url('/admin/users/:id/password'), endpoint(async ({ request, params }) => {
    const user = requirePermission('users.manage')
    const body = (await request.json()) as Record<string, unknown>
    ops.setPassword(user, String(params.id), body.password, demoSettings.minPasswordLength)
    return new HttpResponse(null, { status: 204 })
  }, { mutates: true })),

  // Demo only

  http.post(url('/demo/reset'), endpoint(() => {
    currentUser()
    resetDb(Date.now())
    markChanged('deliveries', 'notifications')
    return new HttpResponse(null, { status: 204 })
  }, { mutates: true })),
]
