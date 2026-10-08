// Demo versions of the platform endpoints: services, tasks, overview, schedule, activity, maintenance, search and the
// admin views. The same rules as backend/eiu_web_backend/dashboard.py.
import type {
  AccessCatalog, ActivityView, AlertList, Catalog, IntegrationCard, MaintenanceItem, MaintenanceView, NewTask, OperationsOverview, RobotGroup,
  ScheduleItem, ScheduleStatus, SearchResults, ServiceCardView, ServiceDef, SystemSettings, TaskDetail, TaskState,
} from '../api/types'
import {
  ADMIN_PERMISSIONS, DEFAULT_OPERATOR, FLEETS, Forbidden, OPERATOR_PERMISSIONS, SERVICES, capableFleets, checkRobot, checkScope, checkService,
  fleetVisible, require, seesAll, serviceEnabled, serviceOf, servicesOf, taskVisible, validate, zoneEnabled, zonesOf,
} from './access'
import { db, save } from './db'
import * as ops from './operations'
import type { DeliveryRow, UserRow } from './schema'
import { demoSettings } from './settings'
import { cancel, markChanged, notify, setPaused, tick } from './simulator'
import { ACTIVE, FINAL, taskState, toDelivery, toLocation } from './views'

const DAY = 86_400_000
const ROBOT_GROUPS: Record<string, RobotGroup> = {
  NAVIGATING: 'active', EXECUTING: 'active', IDLE: 'idle', CHARGING: 'charging', PAUSED: 'paused', MAINTENANCE: 'maintenance', OFFLINE: 'offline', ERROR: 'error',
}
const SCHEDULE_STATUS: Record<TaskState, ScheduleStatus> = {
  COMPLETED: 'completed', EXECUTING: 'in_progress', PAUSED: 'in_progress', SCHEDULED: 'scheduled', QUEUED: 'pending', ASSIGNED: 'pending',
  FAILED: 'failed', CANCELLED: 'cancelled',
}

function dayStart(ms: number): number {
  const d = new Date(ms)
  d.setHours(0, 0, 0, 0)
  return d.getTime()
}

export function services(user: UserRow): ServiceDef[] {
  const allowed = new Set(servicesOf(user))
  return SERVICES.filter((s) => user.role === 'admin' || (serviceEnabled(s) && allowed.has(s.id)))
    .map((s) => ({ ...s, enabled: serviceEnabled(s), allowed: allowed.has(s.id), available: capableFleets(s).length > 0, fleets: capableFleets(s) }))
}

export function catalog(user: UserRow): Catalog {
  const zones = zonesOf(user)
  const data = db()
  return { zones: data.zones.map((z) => ({ ...z, enabled: zoneEnabled(z.id), allowed: zones === null || zones.has(z.id) })), areas: data.areas, routes: data.routes }
}

// Tasks

function fillPlaces(row: DeliveryRow): void {
  const p = row.params
  if (row.kind === 'delivery') {
    row.pickupId = String(p.pickup)
    row.dropoffId = String(p.dropoff)
    row.packageType = String(p.itemType ?? 'general') as DeliveryRow['packageType']
  } else if (row.kind === 'patrol') {
    const route = db().routes.find((r) => r.id === p.route)
    row.stops = route ? [...route.stops] : ((p.stops as string[]) ?? [])
    row.rounds = Number(p.rounds ?? 1)
    if (row.rounds > 1 && row.stops.length < 2) throw new Forbidden(422, 'patrol.needs_two_stops')
    row.pickupId = row.stops[0]
    row.dropoffId = row.stops[row.stops.length - 1]
  }
}

export function createTask(user: UserRow, body: Partial<NewTask>, previous?: DeliveryRow): DeliveryRow {
  const data = db()
  const now = Date.now()
  const service = checkService(user, serviceOf(String(body.serviceType ?? '')))
  const { values, zones } = previous ? { values: previous.params, zones: new Set(previous.zones) } : validate(service, (body.parameters ?? {}) as Record<string, unknown>)
  checkScope(user, service, zones)
  const scheduledAt = body.scheduledAt ?? null
  const repeat = body.repeat ?? 'none'
  const schedule = service.taskFormSchema.find((f) => f.type === 'schedule')
  if (scheduledAt !== null) {
    if (!schedule) throw new Forbidden(422, 'task.schedule_not_allowed')
    require(user, 'task.schedule')
    if (scheduledAt < now + 60_000 || scheduledAt > now + 14 * DAY) throw new Forbidden(422, 'delivery.invalid_schedule')
  }
  if (repeat !== 'none' && (scheduledAt === null || !schedule?.repeat)) throw new Forbidden(422, 'task.invalid_repeat')
  if (body.robot) {
    const robot = data.robots.find((r) => r.name === body.robot)
    if (!robot) throw new Forbidden(404, 'robot.not_found')
    checkRobot(user, service, robot.name, robot.fleet, previous ? 'task.reassign' : 'fleet.assign')
  }
  if (!previous && data.deliveries.filter((d) => d.requesterId === user.id && (ACTIVE.has(d.status) || d.status === 'scheduled')).length >= demoSettings.maxActiveDeliveries) {
    throw new Forbidden(409, 'delivery.limit_reached')
  }
  const row: DeliveryRow = {
    id: data.nextDeliveryId++, kind: service.category, service: service.id, params: values, zones: [...zones].sort(),
    priority: String(values.priority ?? 'normal'), repeat, previousId: previous?.id ?? null, requestedRobot: body.robot ?? null,
    startedAt: null, paused: false, requesterId: previous?.requesterId ?? user.id, pickupId: '', dropoffId: '', stops: [], rounds: 1, roundsDone: 0,
    packageType: 'general', note: String(values.note ?? ''), createdAt: now, scheduledAt, status: scheduledAt === null ? 'queued' : 'scheduled',
    robotName: null, plan: null, events: [{ type: 'requested', at: now, detail: body.robot ?? '' }], finishedAt: null,
  }
  fillPlaces(row)
  data.deliveries.push(row)
  if (!previous) notify(row, 'request_received', now)
  ops.audit(user, 'task.create', `${row.service}#${row.id}`)
  markChanged('deliveries')
  tick(now)
  save()
  return row
}

function visibleRow(user: UserRow, id: number): DeliveryRow {
  const row = db().deliveries.find((d) => d.id === id)
  if (!row || !taskVisible(user, row)) throw new Forbidden(404, 'delivery.not_found')
  return row
}

export function taskDetail(user: UserRow, id: number): TaskDetail {
  const data = db()
  const row = visibleRow(user, id)
  const audit = (data.audit ?? []).filter((a) => a.target === `${row.service}#${row.id}` && a.action !== 'task.create')
  const names = new Map(data.users.map((u) => [u.id, u.fullName]))
  const activity = [
    ...row.events.map((e) => ({ at: e.at, type: e.type, detail: e.detail ?? '' })),
    ...audit.map((a) => ({ at: a.at, type: a.action, detail: names.get(a.actorId ?? '') ?? '' })),
  ].sort((a, b) => a.at - b.at)
  return { ...toDelivery(data, row, user, true), rmfTaskId: `${row.kind}.dispatch-${row.id}`, activity }
}

export function taskAction(user: UserRow, id: number, action: string, robot?: string): TaskDetail {
  const row = visibleRow(user, id)
  const dto = toDelivery(db(), row, user, true)
  const now = Date.now()
  if (action === 'cancel') {
    require(user, 'task.cancel')
    if (!dto.actions.cancel) throw new Forbidden(409, 'delivery.not_cancellable')
    cancel(row, now)
  } else if (action === 'pause' || action === 'resume') {
    require(user, 'task.pause')
    if (!dto.actions[action]) throw new Forbidden(409, action === 'pause' ? 'task.not_pausable' : 'task.not_resumable')
    setPaused(row, action === 'pause', now)
  } else if (action === 'reassign') {
    require(user, 'task.reassign')
    if (!dto.actions.reassign) throw new Forbidden(409, 'task.not_reassignable')
    const next = createTask(user, { serviceType: row.service, scheduledAt: row.scheduledAt && row.scheduledAt > now + 60_000 ? row.scheduledAt : null,
      repeat: row.repeat, robot }, row)
    cancel(row, now)
    row.events.push({ type: 'reassigned', at: now, detail: String(next.id) })
    save()
    return taskDetail(user, next.id)
  } else {
    throw new Forbidden(404, 'task.unknown_action')
  }
  ops.audit(user, `task.${action}`, `${row.service}#${row.id}`)
  return taskDetail(user, id)
}

// Alerts

export function visibleAlerts(user: UserRow, state: string): AlertList {
  const listing = ops.alerts(state)
  const data = db()
  const fleets = new Map(data.robots.map((r) => [r.name, r.fleet]))
  const items = listing.items.filter((a) => {
    if (seesAll(user)) return true
    if (a.deliveryId !== null) {
      const row = data.deliveries.find((d) => d.id === a.deliveryId)
      return !!row && servicesOf(user).includes(row.service)
    }
    const fleet = a.robot ? fleets.get(a.robot) : (a.params.fleet as string | undefined)
    return !!fleet && fleetVisible(user, fleet)
  })
  const open = items.filter((a) => a.resolvedAt === null)
  return { items, counts: { open: open.length, unacked: open.filter((a) => a.ackedAt === null).length, critical: open.filter((a) => a.severity === 'critical').length } }
}

// Overview

export function overview(user: UserRow): OperationsOverview {
  const now = Date.now()
  const today = dayStart(now)
  const robots = ops.fleetRobots(now, user)
  const online = robots.filter((r) => r.connection === 'online')
  const rows = db().deliveries.filter((d) => taskVisible(user, d) && (!FINAL.has(d.status) || (d.finishedAt ?? 0) >= today || d.createdAt >= today))
  const states = rows.map((r) => [r, taskState(r)] as const)
  const completed = states.filter(([r, s]) => s === 'COMPLETED' && (r.finishedAt ?? 0) >= today).length
  const failed = states.filter(([r, s]) => s === 'FAILED' && (r.finishedAt ?? 0) >= today).length
  const open = new Set<TaskState>(['QUEUED', 'ASSIGNED', 'EXECUTING', 'PAUSED'])
  const attention = visibleAlerts(user, 'open')
  const groups = (list: typeof robots) => {
    const out = { active: 0, idle: 0, charging: 0, paused: 0, maintenance: 0, offline: 0, error: 0 } as Record<RobotGroup, number>
    list.forEach((r) => { out[ROBOT_GROUPS[r.status]] += 1 })
    return out
  }
  const ids = user.role === 'admin' ? SERVICES.map((s) => s.id) : servicesOf(user)
  const cards: ServiceCardView[] = ids.map((id) => {
    const svc = serviceOf(id)!
    const own = states.filter(([r]) => r.service === id)
    const members = robots.filter((r) => r.services.includes(id))
    const metrics: ServiceCardView['metrics'] = [{ key: 'completedToday', value: own.filter(([r, s]) => s === 'COMPLETED' && (r.finishedAt ?? 0) >= today).length }]
    if (svc.category === 'patrol') metrics.push({ key: 'roundsToday', value: own.reduce((n, [r]) => n + r.roundsDone, 0) })
    return { id, enabled: serviceEnabled(svc), available: capableFleets(svc).length > 0, robots: members.length, byStatus: groups(members),
      tasksToday: own.filter(([r]) => r.createdAt >= today).length, activeTasks: own.filter(([, s]) => open.has(s)).length, metrics }
  })
  const other = robots.filter((r) => r.services.length === 0)
  if (other.length && seesAll(user)) cards.push({ id: 'other', enabled: true, available: true, robots: other.length, byStatus: groups(other), tasksToday: 0, activeTasks: 0, metrics: [] })
  return {
    robots: { total: robots.length, online: online.length, available: online.filter((r) => r.status === 'IDLE').length,
      avgBattery: online.length ? Math.round((online.reduce((n, r) => n + r.battery, 0) / online.length) * 10) / 10 : null },
    tasks: { active: states.filter(([, s]) => open.has(s)).length, executing: states.filter(([, s]) => s === 'EXECUTING').length,
      scheduled: states.filter(([, s]) => s === 'SCHEDULED').length, completedToday: completed, failedToday: failed,
      successRate: completed + failed ? Math.round((1000 * completed) / (completed + failed)) / 10 : null },
    attention: attention.counts,
    alerts: attention.items.filter((a) => a.resolvedAt === null).slice(0, 8),
    services: cards,
    schedule: schedule(user, today, today + DAY, {}).items,
    ...(user.role === 'admin' ? { health: ops.overview(now).health } : {}),
  }
}

// Schedule and activity

export function schedule(user: UserRow, start: number, end: number, f: { service?: string; robot?: string; zone?: string }) {
  if (!(end > start) || end - start > 62 * DAY) throw new Forbidden(422, 'schedule.invalid_range')
  const data = db()
  const names = new Set(ops.fleetRobots(Date.now(), user).map((r) => r.name))
  const items: ScheduleItem[] = []
  for (const r of data.deliveries) {
    if (!taskVisible(user, r)) continue
    const at = r.scheduledAt ?? r.startedAt ?? r.createdAt
    if (at < start || at >= end || (f.service && r.service !== f.service) || (f.robot && r.robotName !== f.robot) || (f.zone && !r.zones.includes(f.zone))) continue
    items.push({ type: 'task', id: `task-${r.id}`, taskId: r.id, at, end: r.finishedAt, service: r.service, robot: r.robotName, zones: r.zones,
      status: SCHEDULE_STATUS[taskState(r)], repeat: r.repeat, pickupId: r.pickupId, dropoffId: r.dropoffId,
      areaId: (r.params.area as string) ?? null, routeId: (r.params.route as string) ?? null })
  }
  if (!f.service) {
    for (const m of data.maintenance) {
      const at = m.windowStart ?? m.dueAt
      if (at === null || at < start || at >= end || !names.has(m.robot) || (f.robot && m.robot !== f.robot) || m.status === 'cancelled') continue
      items.push({ type: 'maintenance', id: `maintenance-${m.id}`, maintenanceId: m.id, at, end: m.windowEnd, robot: m.robot, title: m.title,
        status: m.status === 'done' ? 'completed' : m.status === 'in_progress' ? 'in_progress' : 'scheduled', service: null, zones: [] })
    }
    // The demo keeps no history: a robot charging now is shown as a session that began half an hour ago.
    const now = Date.now()
    for (const r of ops.fleetRobots(now, user).filter((x) => x.status === 'CHARGING' && (!f.robot || x.name === f.robot))) {
      const at = Math.max(now - 30 * 60_000, start)
      if (at < end && now >= start) items.push({ type: 'charging', id: `charging-${r.name}`, at, end: null, robot: r.name, status: 'in_progress', service: null, zones: [] })
    }
  }
  items.sort((a, b) => a.at - b.at)
  return { from: start, to: end, items }
}

export function activity(user: UserRow, range: string, service: string): ActivityView {
  const now = Date.now()
  const today = dayStart(now)
  const hourly = range === 'today'
  const count = hourly ? 24 : range === 'week' ? 7 : 30
  if (!['today', 'week', 'month'].includes(range)) throw new Forbidden(422, 'activity.invalid_range')
  const bounds = Array.from({ length: count + 1 }, (_, i) => {
    const d = new Date(hourly ? today : today - (count - 1) * DAY)
    if (hourly) d.setHours(i)
    else d.setDate(d.getDate() + i)
    return d.getTime()
  })
  const rows = db().deliveries.filter((r) => taskVisible(user, r) && (!service || r.service === service))
  return {
    range: range as ActivityView['range'], unit: hourly ? 'hour' : 'day',
    buckets: bounds.slice(0, -1).map((lo, i) => {
      const hi = bounds[i + 1]
      return {
        start: lo,
        created: rows.filter((r) => r.createdAt >= lo && r.createdAt < hi).length,
        running: lo <= now ? rows.filter((r) => r.startedAt !== null && r.startedAt < hi && (r.finishedAt ?? now) >= lo).length : 0,
        completed: rows.filter((r) => r.status === 'completed' && (r.finishedAt ?? 0) >= lo && (r.finishedAt ?? 0) < hi).length,
        failed: rows.filter((r) => r.status === 'failed' && (r.finishedAt ?? 0) >= lo && (r.finishedAt ?? 0) < hi).length,
      }
    }),
  }
}

// Maintenance

export function maintenanceView(user: UserRow): MaintenanceView {
  const now = Date.now()
  const robots = ops.fleetRobots(now, user)
  const names = new Set(robots.map((r) => r.name))
  const items = db().maintenance.filter((m) => names.has(m.robot))
  return {
    items,
    robots: robots.map((r) => {
      const open = items.filter((m) => m.robot === r.name && (m.status === 'planned' || m.status === 'in_progress'))
        .sort((a, b) => Number(a.status !== 'in_progress') - Number(b.status !== 'in_progress') || (a.dueAt ?? a.windowStart ?? 1e15) - (b.dueAt ?? b.windowStart ?? 1e15))
      const next = open[0] ?? null
      const status = r.maintenance ? 'in_progress' : next?.dueAt != null && next.dueAt <= now ? 'due'
        : next?.dueAt != null && next.dueAt - now <= 3 * DAY ? 'due_soon' : next ? 'scheduled' : 'none'
      const done = items.filter((m) => m.robot === r.name && m.doneAt).map((m) => m.doneAt!)
      const since = done.length ? Math.max(...done) : now - 30 * DAY
      return { robot: r.name, serviceType: r.serviceType, health: r.health, status, next, openItems: open.length,
        operatingHours: Math.round(((now - since) / 3_600_000) * 0.35 * 10) / 10, issue: next?.title ?? null }
    }),
  }
}

export function saveMaintenance(user: UserRow, id: number | null, body: Partial<MaintenanceItem>): MaintenanceItem {
  require(user, 'maintenance.manage')
  const data = db()
  if (id === null) {
    if (!data.robots.some((r) => r.name === body.robot)) throw new Forbidden(404, 'robot.not_found')
    const title = String(body.title ?? '').trim()
    if (!title) throw new Forbidden(422, 'maintenance.invalid')
    if (body.windowStart != null && (body.windowEnd == null || body.windowEnd <= body.windowStart)) throw new Forbidden(422, 'maintenance.invalid_window')
    const item: MaintenanceItem = { id: data.nextMaintenanceId++, robot: String(body.robot), title, status: 'planned', dueAt: body.dueAt ?? null,
      windowStart: body.windowStart ?? null, windowEnd: body.windowEnd ?? null, note: String(body.note ?? ''), createdBy: user.fullName,
      createdAt: Date.now(), doneAt: null, doneBy: null }
    data.maintenance.push(item)
    ops.audit(user, 'maintenance.create', item.robot, title)
    markChanged('fleet')
    save()
    return item
  }
  const item = data.maintenance.find((m) => m.id === id)
  if (!item) throw new Forbidden(404, 'maintenance.not_found')
  if (body.status) {
    if (item.status === 'done' || item.status === 'cancelled') throw new Forbidden(409, 'maintenance.closed')
    item.status = body.status
    if (body.status === 'done') Object.assign(item, { doneAt: Date.now(), doneBy: user.fullName })
  }
  ops.audit(user, 'maintenance.update', item.robot, `#${item.id} ${item.status}`)
  markChanged('fleet', 'alerts')
  save()
  return item
}

// Search

export function search(user: UserRow, q: string): SearchResults {
  const needle = q.trim().toLowerCase()
  const data = db()
  if (!needle) return { robots: [], tasks: [], locations: [], zones: [], users: [] }
  const has = (...texts: string[]) => texts.some((t) => t.toLowerCase().includes(needle))
  return {
    robots: ops.fleetRobots(Date.now(), user).filter((r) => has(r.name, r.fleet)).slice(0, 6)
      .map((r) => ({ name: r.name, fleet: r.fleet, serviceType: r.serviceType, status: r.status })),
    tasks: ops.tasks(user, 'all', '', needle).items.slice(0, 6).map((t) => ({ id: t.id, service: t.service, state: t.state, pickup: t.pickup, dropoff: t.dropoff, area: t.area, route: t.route })),
    locations: data.locations.filter((l) => has(l.id, l.name.vi, l.name.en)).slice(0, 6).map(toLocation),
    zones: data.zones.filter((z) => has(z.id, z.name.vi, z.name.en)).slice(0, 6).map((z) => ({ id: z.id, name: z.name })),
    users: user.role === 'admin' ? data.users.filter((u) => has(u.fullName, u.email)).slice(0, 6).map((u) => ({ id: u.id, fullName: u.fullName, email: u.email, role: u.role })) : [],
  }
}

// Administration

export function accessCatalog(): AccessCatalog {
  const data = db()
  return {
    services: SERVICES.map((s) => ({ id: s.id, name: s.name, icon: s.icon, enabled: serviceEnabled(s) })),
    zones: data.zones.map((z) => ({ id: z.id, name: z.name, enabled: zoneEnabled(z.id) })),
    permissionGroups: Object.entries(OPERATOR_PERMISSIONS).map(([id, permissions]) => ({ id: id as 'task', permissions })),
    adminPermissions: ADMIN_PERMISSIONS as AccessCatalog['adminPermissions'],
    defaults: DEFAULT_OPERATOR,
  }
}

export function setToggle(user: UserRow, kind: string, id: string, enabled: unknown) {
  require(user, 'settings.manage')
  const known = kind === 'service' ? SERVICES.map((s) => s.id) : db().zones.map((z) => z.id)
  if (!['service', 'zone'].includes(kind) || !known.includes(id) || typeof enabled !== 'boolean') throw new Forbidden(422, 'settings.invalid')
  db().toggles[`${kind}:${id}`] = enabled
  ops.audit(user, `${kind}.${enabled ? 'enable' : 'disable'}`, id)
  markChanged('access')
  save()
  return { kind, id, enabled }
}

export function systemSettings(): SystemSettings {
  return {
    settings: { site: { name: 'EIU Robot Services', time_zone: demoSettings.timeZone }, delivery: { max_open: demoSettings.maxActiveDeliveries },
      demo: { tick_ms: demoSettings.tickMs, robot_speed_mps: demoSettings.robotSpeedMps } },
    services: SERVICES.map((s) => ({ ...s, enabled: serviceEnabled(s), configured: s.enabled })),
    zones: db().zones.map((z) => ({ ...z, enabled: zoneEnabled(z.id), configured: z.enabled, allowed: true })),
    fleets: FLEETS,
    templates: db().templates,
    sources: { settings: 'demo', catalog: 'demo seed', services: 'mocks/access.ts' },
  }
}

export function integrations(): IntegrationCard[] {
  const now = Date.now()
  const system = ops.system(now)
  const fleets = Object.keys(FLEETS)
  const logs = (prefix: string) => ops.alerts('all').items.filter((a) => a.code.startsWith(prefix)).slice(0, 8)
    .map((a) => ({ at: a.openedAt, level: a.severity, code: a.code, params: a.params, closedAt: a.resolvedAt }))
  return [
    { id: 'open_rmf', status: 'ok', lastUpdateAt: now - 400, fleets, config: { requester: 'eiu_web_dashboard', fleet_offline_s: 5, task_events: true }, logs: logs('rmf.') },
    { id: 'ros2_gateway', status: 'ok', lastUpdateAt: now - 300, fleets, config: { redis: 'demo', prefix: 'eiu:rmf', ros_domain: 'demo' }, logs: [] },
    { id: 'vda5050', status: 'ok', lastUpdateAt: now - 1000, fleets, config: { demo_fleet_adapter: fleets.join(', ') }, logs: logs('adapter.'), adapters: system.adapters },
    { id: 'mqtt', status: 'ok', lastUpdateAt: null, fleets, config: { demo_fleet_adapter: { connected: true, connections_lost: 0 } }, logs: [] },
    { id: 'vendor', status: 'not_configured', lastUpdateAt: null, fleets: [], config: {}, logs: [] },
  ]
}

export { Forbidden, require }
