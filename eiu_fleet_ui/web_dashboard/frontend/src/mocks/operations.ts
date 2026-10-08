// Demo stand-in for the fleet adapter side of the operator features; state lives in memory for the page.
import type {
  AnalyticsReport,
  RobotDetailView,
  RobotEvent,
  TaskRef,
  UtilizationDay,
  AdminOverview,
  AdminUser,
  AlertList,
  AuditList,
  Infrastructure,
  AlertSeverity,
  FleetRobot,
  FleetTaskList,
  HealthItem,
  OpsAlert,
  Overview,
  TaskGroup,
  GraphLaneEdit,
  GraphVertexEdit,
  LanesView,
  NavGraphView,
  PendingRobot,
  RegistrationResult,
  RegistrationView,
  SystemView,
} from '../api/types'
import type { LiveTask, RobotStatus, TaskListView, TaskState } from '../api/types'
import { DEFAULT_OPERATOR, GRANTABLE, SERVICES, fleetCapabilities, fleetServices, fleetVisible, permissionsOf, taskVisible } from './access'
import { db, save } from './db'
import type { DeliveryRow, UserRow } from './schema'
import { cancel, inMaintenance, liveRobots, markChanged } from './simulator'
import { siteLevels } from './site'
import { ACTIVE, FINAL, taskState, toDelivery } from './views'

const NAV_GRAPH_PATH = 'demo/nav_graph.yaml'
const DEMO_SERIES = 'demo_amr'

const paused = new Set<string>()
const speedLimits = new Map<string, number>()
const closedLanes = new Set<number>()
let graphVersion = 1
const startedAt = Date.now()
const parking = siteLevels[0]?.graph.vertices.find((v) => v.charger) ?? siteLevels[0]?.graph.vertices[0]
const discovered: PendingRobot[] = [{
  manufacturer: 'EIU', serial: '0009', series: DEMO_SERIES,
  pose: { x: parking?.x ?? 0, y: parking?.y ?? 0, map: siteLevels[0]?.id ?? '', initialized: true },
  reporters: ['demo_fleet_adapter'],
  suggestion: { fleet: '', name: '', charger: '' },
}]

const sha = () => `demo-${graphVersion}`

export class OperationError extends Error {
  constructor(readonly status: number, readonly code: string) {
    super(code)
  }
}

function fleets(): string[] {
  return [...new Set(db().robots.map((r) => r.fleet))].sort()
}

function chargers(levelId: string) {
  return (siteLevels.find((l) => l.id === levelId)?.graph.vertices ?? []).filter((v) => v.charger && v.name).map((v) => v.name)
}

const onlineSince = new Map<string, number>()

function liveTask(row: DeliveryRow): LiveTask {
  return { id: row.id, service: row.service, kind: row.kind, state: taskState(row), status: row.status, pickupId: row.pickupId,
    dropoffId: row.dropoffId, stops: row.stops, areaId: (row.params.area as string) ?? null, routeId: (row.params.route as string) ?? null,
    rounds: row.rounds, roundsDone: row.roundsDone, etaAt: row.plan ? Math.round(row.kind === 'delivery' ? row.plan.arriveAt : row.plan.completeAt) : null,
    zones: row.zones }
}

/** Telemetry the demo robots report, by service; the real backend shows only what a robot reports. */
function telemetry(fleet: string, name: string, row: DeliveryRow | undefined, now: number): FleetRobot['telemetry'] {
  const seed = [...name].reduce((a, c) => a + c.charCodeAt(0), 0)
  if (fleet === 'eiu_cleaning') {
    const progress = row?.plan ? Math.min(100, Math.round(((now - row.plan.startAt) / Math.max(1, row.plan.completeAt - row.plan.startAt)) * 100)) : 0
    return { water_tank: 40 + (seed % 50), waste_tank: 10 + (seed % 30), brush_health: name === 'CLN-01' ? 18 : 76,
      cleaning_mode: (row?.params.cleaningMode as string) ?? 'idle', ...(row ? { coverage: progress } : {}) }
  }
  if (fleet === 'eiu_patrol') return { camera: 'ok', ...(row ? { checkpoint: row.stops[row.roundsDone % Math.max(1, row.stops.length)] ?? '' } : {}) }
  return { compartment: row && row.status === 'in_transit' ? 'locked' : 'closed', payload: row ? String(row.packageType) : 'empty', qr_pin: 'not_used' }
}

function statusOf(r: { activity: string; mode: string; paused: boolean }, task: LiveTask | null, maintenance: boolean): RobotStatus {
  if (r.activity === 'offline') return 'OFFLINE'
  if (maintenance) return 'MAINTENANCE'
  if (r.paused || task?.state === 'PAUSED') return 'PAUSED'
  if (r.mode === 'charging') return 'CHARGING'
  if (task?.state === 'EXECUTING') return task.kind !== 'delivery' || r.activity !== 'moving' ? 'EXECUTING' : 'NAVIGATING'
  return r.activity === 'moving' ? 'NAVIGATING' : 'IDLE'
}

export function fleetRobots(now: number, user?: UserRow): FleetRobot[] {
  const data = db()
  const levels = alertLevels(now)
  return liveRobots(now).filter((r) => !user || fleetVisible(user, r.fleet)).map((r) => {
    const row = r.deliveryId !== null ? data.deliveries.find((d) => d.id === r.deliveryId) : undefined
    const task = row ? liveTask(row) : null
    const docked = r.activity === 'idle' && data.robots.find((x) => x.name === r.name)?.battery !== undefined && atCharger(r.x, r.y)
    const mode = docked ? 'charging' : r.activity
    const maintenance = inMaintenance(r.name, now)
    if (!onlineSince.has(r.name)) onlineSince.set(r.name, now - (2 * 3600_000 + ([...r.name].reduce((a, c) => a + c.charCodeAt(0), 0) % 7200) * 1000))
    const status = statusOf({ activity: r.activity, mode, paused: paused.has(r.name) }, task, maintenance)
    const level = levels.get(r.name)
    const near = data.locations.reduce<{ id: string; d: number } | null>((best, l) => {
      const v = siteLevels[0]?.graph.vertices.find((x) => x.name === l.waypoint)
      if (!v) return best
      const d = Math.hypot(v.x - r.x, v.y - r.y)
      return !best || d < best.d ? { id: l.id, d } : best
    }, null)
    return {
      name: r.name, fleet: r.fleet, levelId: r.levelId, x: r.x, y: r.y, yaw: r.yaw, battery: r.battery,
      activity: paused.has(r.name) ? 'waiting' : r.activity, mode, paused: paused.has(r.name),
      taskId: r.deliveryId ? `demo-${r.deliveryId}` : null, deliveryId: r.deliveryId,
      adapter: 'demo_fleet_adapter', controls: true, speedLimit: speedLimits.get(r.name) ?? 0,
      status, health: status === 'OFFLINE' || status === 'ERROR' || level === 'critical' ? 'critical' : level === 'warning' || maintenance ? 'warning' : 'healthy',
      serviceType: fleetServices(r.fleet)[0] ?? null, services: fleetServices(r.fleet), capabilities: fleetCapabilities(r.fleet),
      connection: r.activity === 'offline' ? 'offline' : 'online', onlineSince: onlineSince.get(r.name) ?? null, lastUpdateAt: now,
      locationId: near?.id ?? null, task, maintenance, telemetry: telemetry(r.fleet, r.name, row, now),
    }
  })
}

function atCharger(x: number, y: number): boolean {
  return (siteLevels[0]?.graph.vertices ?? []).some((v) => v.charger && Math.hypot(v.x - x, v.y - y) < 0.05)
}

/** Worst open condition per robot, for its health. */
function alertLevels(now: number): Map<string, AlertSeverity> {
  const out = new Map<string, AlertSeverity>()
  for (const c of conditions(now, false).values()) {
    if (c.robot && (c.severity === 'critical' || !out.has(c.robot))) out.set(c.robot, c.severity)
  }
  return out
}

export function robotCommand(robot: string, action: string, body: Record<string, unknown>): { ok: boolean; message: string } {
  const row = db().robots.find((r) => r.name === robot)
  if (!row) throw new OperationError(404, 'robot.not_found')
  if (action === 'pause') paused.add(robot)
  else if (action === 'resume') paused.delete(robot)
  else if (action === 'speed-limit') {
    const mps = body.mps
    if (typeof mps !== 'number' || !Number.isFinite(mps) || mps < 0) throw new OperationError(422, 'robot.invalid_speed')
    speedLimits.set(robot, mps)
  } else if (action === 'init-position') {
    const vertex = siteLevels.find((l) => l.id === row.levelId)?.graph.vertices.find((v) => v.name === body.waypoint)
    if (!vertex) throw new OperationError(422, 'robot.unknown_waypoint')
    if (row.deliveryId === null) {
      row.x = vertex.x
      row.y = vertex.y
      row.yaw = typeof body.yaw === 'number' ? body.yaw : row.yaw
      save()
    }
  } else throw new OperationError(404, 'robot.unknown_action')
  markChanged('fleet')
  return { ok: true, message: '' }
}

export function registration(): RegistrationView {
  const robots = db().robots
  const levelId = siteLevels[0]?.id ?? ''
  const free = chargers(levelId)
  const views = fleets().map((fleet) => {
    const own = robots.filter((r) => r.fleet === fleet)
    const assigned = own.map((r, i) => ({ name: r.name, manufacturer: 'EIU', serial: String(i + 1).padStart(4, '0'), charger: free[i] ?? '', source: 'config' }))
    return {
      fleet, series: DEMO_SERIES, adapter_node: 'demo_fleet_adapter', interface: 'AMR',
      limits: { linear_speed: 0.25, linear_acceleration: 0.5, footprint_radius: 0.3, tolerance: 0.05 },
      robots: assigned,
      chargers: free.map((name) => ({ name, used_by: assigned.find((r) => r.charger === name)?.name })),
    }
  })
  const first = views[0]
  const registered = new Set(db().registeredSerials ?? [])
  const pending = discovered.filter((p) => !registered.has(p.serial)).map((p) => {
    const taken = new Set(robots.map((r) => r.name))
    let n = robots.length + 1
    while (taken.has(`robot_${n}`)) n++
    const suggestion = { fleet: first?.fleet ?? '', name: `robot_${n}`, charger: first?.chargers[0]?.name ?? '' }
    return { ...p, pose: p.pose && { ...p.pose, map: levelId }, suggestion }
  })
  return { fleets: views, pending }
}

export function register(body: Record<string, unknown>): RegistrationResult {
  const action = body.action
  const fleet = typeof body.fleet === 'string' ? body.fleet : ''
  const name = typeof body.name === 'string' ? body.name.trim() : ''
  if (!['check', 'add', 'remove'].includes(String(action)) || !fleet || !name) throw new OperationError(422, 'registration.invalid')
  const robots = db().robots
  const result = (errors: RegistrationResult['errors'], dryRun: boolean): RegistrationResult =>
    ({ ok: errors.length === 0, dryRun, persisted: false, needsConfirmation: false, errors, warnings: [] })

  if (action === 'remove') {
    const index = robots.findIndex((r) => r.name === name && r.fleet === fleet)
    if (index < 0) return result([{ code: 'unknown_robot', message: `${name} is not in ${fleet}` }], false)
    if (robots[index].deliveryId !== null) return result([{ code: 'busy', message: `${name} has a task` }], false)
    robots.splice(index, 1)
    save()
    markChanged('fleet')
    return result([], false)
  }
  const errors: RegistrationResult['errors'] = []
  if (!fleets().includes(fleet)) errors.push({ code: 'unknown_fleet', message: `No fleet ${fleet}` })
  if (robots.some((r) => r.name === name)) errors.push({ code: 'name_taken', message: `${name} is already registered` })
  const found = discovered.findIndex((p) => p.manufacturer === body.manufacturer && p.serial === body.serial
    && !(db().registeredSerials ?? []).includes(p.serial))
  if (found < 0) errors.push({ code: 'not_discovered', message: 'The robot is not on the broker' })
  if (action === 'check' || errors.length > 0) return result(errors, action === 'check')
  const levelId = siteLevels[0]?.id ?? ''
  const pose = discovered[found].pose
  robots.push({ name, fleet, levelId, x: pose?.x ?? 0, y: pose?.y ?? 0, yaw: 0, battery: 100, deliveryId: null })
  db().registeredSerials = [...(db().registeredSerials ?? []), discovered[found].serial]
  save()
  markChanged('fleet')
  return result([], false)
}

function laneOffsets(): Record<string, number> {
  let offset = 0
  const out: Record<string, number> = {}
  for (const level of siteLevels) {
    out[level.id] = offset
    offset += level.graph.lanes.length
  }
  return out
}

export function lanes(): LanesView {
  const closed = [...closedLanes].sort((a, b) => a - b)
  return { fleets: Object.fromEntries(fleets().map((f) => [f, closed])), offsets: laneOffsets() }
}

export function setLanes(body: { close?: unknown; open?: unknown }): { ok: boolean; fleets: string[] } {
  const total = siteLevels.reduce((n, l) => n + l.graph.lanes.length, 0)
  const close = Array.isArray(body.close) ? body.close : []
  const open = Array.isArray(body.open) ? body.open : []
  if (close.length + open.length === 0 || ![...close, ...open].every((i) => Number.isInteger(i) && i >= 0 && i < total)) {
    throw new OperationError(422, 'lanes.invalid')
  }
  close.forEach((i: number) => closedLanes.add(i))
  open.forEach((i: number) => closedLanes.delete(i))
  markChanged('fleet')
  return { ok: true, fleets: fleets() }
}

export function navGraph(levelId: string | null): NavGraphView {
  const level = siteLevels.find((l) => l.id === levelId) ?? siteLevels[0]
  if (!level) return { available: false }
  return {
    available: true, path: NAV_GRAPH_PATH, sha256: sha(), levels: siteLevels.map((l) => l.id), levelId: level.id,
    vertices: level.graph.vertices.map((v) => ({ ...v, attrs: {} })),
    lanes: level.graph.lanes.map(([from, to]) => ({ from, to, attrs: {} })),
  }
}

export function saveNavGraph(body: { levelId?: string; baseSha256?: string; vertices?: GraphVertexEdit[]; lanes?: GraphLaneEdit[] }) {
  if (body.baseSha256 !== sha()) throw new OperationError(409, 'nav_graph.stale')
  const level = siteLevels.find((l) => l.id === body.levelId)
  const { vertices, lanes: edges } = body
  if (!level || !Array.isArray(vertices) || !Array.isArray(edges)) throw new OperationError(422, 'nav_graph.invalid')
  const names = vertices.map((v) => v.name).filter(Boolean)
  if (new Set(names).size !== names.length) throw new OperationError(422, 'nav_graph.duplicate_name')
  if (edges.some((l) => l.from === l.to || !vertices[l.from] || !vertices[l.to])) throw new OperationError(422, 'nav_graph.bad_lane')
  const used = db().locations.filter((l) => l.levelId === level.id).map((l) => l.waypoint)
  if (used.some((w) => !names.includes(w))) throw new OperationError(422, 'nav_graph.catalog_waypoint_missing')
  level.graph = {
    vertices: vertices.map((v) => ({ name: v.name, x: v.x, y: v.y, charger: v.charger })),
    lanes: edges.map((l) => [l.from, l.to] as [number, number]),
  }
  closedLanes.clear()
  graphVersion++
  markChanged('fleet')
  return { ok: true, sha256: sha() }
}

export function system(now: number): SystemView {
  const robots = db().robots
  const ageS = Array.from({ length: 30 }, (_, i) => (29 - i) * 2)
  return {
    rmf: 'online',
    gateway: { ros_domain_id: 'demo', started_ms: startedAt, task_events: true, nav_graph_path: NAV_GRAPH_PATH },
    summary: { total: 1, found: 1, level: 'ok' },
    attention: [],
    adapters: [{
      node: 'demo_fleet_adapter', fleet: fleets()[0] ?? 'demo', status: 'ok', reported_ago_s: 1, interval_s: 2,
      uptime_s: Math.round((now - startedAt) / 1000),
      robots: { registered: robots.length, online: robots.length, state_age_max_s: 0.5, oldest_state_robot: robots[0]?.name ?? '' },
      mqtt: { connected: true, connections_lost: 0 },
      totals: { rx: Math.round((now - startedAt) / 100) * robots.length, unregistered: 0, dropped: 0, published_failed: 0 },
      period_ms: 100,
      series: {
        age_s: ageS,
        msg_per_s: ageS.map((a) => Math.round((10 * robots.length + 2 * Math.sin((now / 1000 - a) / 5)) * 10) / 10),
        loop_p99_ms: ageS.map(() => 1.2),
      },
    }],
  }
}

// Overview, tasks and alerts

const BATTERY_LOW = 20
const BATTERY_CRITICAL = 10
const TASK_WAITING_MS = 10 * 60_000

interface AlertRow extends OpsAlert {
  ackedById: string | null
  resolvedById: string | null
}

let nextAlertId = 1
const alertRows: AlertRow[] = [{
  id: nextAlertId++, key: 'adapter:demo_fleet_adapter:dropped', kind: 'event', severity: 'warning', code: 'adapter.attention',
  params: { title: 'demo_fleet_adapter dropped 2 message(s)', detail: 'In the last report: bad payload 2' },
  robot: null, deliveryId: null, openedAt: startedAt, updatedAt: startedAt, ackedAt: null, ackedBy: null, ackedById: null,
  resolvedAt: null, resolvedBy: null, resolvedById: null, active: true,
}]

function userName(id: string | null): string | null {
  return id ? db().users.find((u) => u.id === id)?.fullName ?? null : null
}

function conditions(now: number, _withTasks = true): Map<string, Pick<OpsAlert, 'severity' | 'code' | 'params' | 'robot' | 'deliveryId'>> {
  const out = new Map<string, Pick<OpsAlert, 'severity' | 'code' | 'params' | 'robot' | 'deliveryId'>>()
  for (const r of liveRobots(now)) {
    if (r.battery < BATTERY_LOW && !(r.activity === 'idle' && atCharger(r.x, r.y))) {
      const severity: AlertSeverity = r.battery < BATTERY_CRITICAL ? 'critical' : 'warning'
      out.set(`robot:${r.name}:battery`, { severity, code: 'robot.battery_low', params: { robot: r.name, battery: Math.round(r.battery) }, robot: r.name, deliveryId: null })
    }
  }
  for (const m of db().maintenance) {
    if (m.status === 'planned' && m.dueAt !== null && m.dueAt <= now) {
      out.set(`maintenance:${m.id}:due`, { severity: 'warning', code: 'maintenance.due', params: { robot: m.robot, title: m.title }, robot: m.robot, deliveryId: null })
    }
  }
  for (const d of db().deliveries) {
    const since = Math.max(d.createdAt, d.scheduledAt ?? 0)
    if (d.status === 'queued' && now - since > TASK_WAITING_MS) {
      out.set(`task:${d.id}:waiting`, { severity: 'warning', code: 'task.waiting', params: { id: d.id, minutes: Math.floor((now - since) / 60_000) }, robot: null, deliveryId: d.id })
    }
  }
  return out
}

/** Opens and closes the condition alerts of the demo; true when the list changed. */
export function tickAlerts(now: number): boolean {
  const current = conditions(now)
  let changed = false
  for (const [key, c] of current) {
    const row = alertRows.find((a) => a.key === key && a.resolvedAt === null)
    if (!row) {
      alertRows.unshift({ id: nextAlertId++, key, kind: 'condition', ...c, openedAt: now, updatedAt: now, ackedAt: null, ackedBy: null,
        ackedById: null, resolvedAt: null, resolvedBy: null, resolvedById: null, active: true })
      changed = true
    } else if (row.severity !== c.severity) {
      Object.assign(row, { severity: c.severity, params: c.params, updatedAt: now })
      changed = true
    }
  }
  for (const row of alertRows) {
    if (row.kind === 'condition' && row.resolvedAt === null && !current.has(row.key)) {
      Object.assign(row, { resolvedAt: now, updatedAt: now, active: false })
      changed = true
    }
  }
  if (changed) markChanged('alerts')
  return changed
}

function alertDto(row: AlertRow): OpsAlert {
  const { ackedById, resolvedById, ...rest } = row
  return { ...rest, ackedBy: userName(ackedById), resolvedBy: userName(resolvedById), active: row.resolvedAt === null }
}

export function alerts(state: string): AlertList {
  tickAlerts(Date.now())
  const open = alertRows.filter((a) => a.resolvedAt === null)
  const rank = { critical: 0, warning: 1, info: 2 }
  const items = (state === 'open' ? [...open].sort((a, b) => Number(a.ackedAt !== null) - Number(b.ackedAt !== null)
    || rank[a.severity] - rank[b.severity] || b.openedAt - a.openedAt) : alertRows).map(alertDto)
  return { items, counts: { open: open.length, unacked: open.filter((a) => a.ackedAt === null).length, critical: open.filter((a) => a.severity === 'critical').length } }
}

export function acknowledge(user: UserRow, id: number | null): number {
  const now = Date.now()
  const rows = alertRows.filter((a) => a.resolvedAt === null && a.ackedAt === null && (id === null || a.id === id))
  if (id !== null && !alertRows.some((a) => a.id === id)) throw new OperationError(404, 'alert.not_found')
  rows.forEach((a) => Object.assign(a, { ackedAt: now, ackedById: user.id, updatedAt: now }))
  markChanged('alerts')
  return rows.length
}

export function resolveAlert(user: UserRow, id: number): void {
  const row = alertRows.find((a) => a.id === id)
  if (!row) throw new OperationError(404, 'alert.not_found')
  if (row.resolvedAt !== null) throw new OperationError(409, 'alert.resolved')
  if (row.kind === 'condition') throw new OperationError(409, 'alert.still_active')
  Object.assign(row, { resolvedAt: Date.now(), resolvedById: user.id, updatedAt: Date.now(), active: false })
  markChanged('alerts')
}

export function overview(now: number): Overview {
  const robots = fleetRobots(now)
  const online = robots.filter((r) => r.activity !== 'offline')
  const dayStart = new Date(now).setHours(0, 0, 0, 0)
  const rows = db().deliveries.filter((d) => !FINAL.has(d.status) || (d.finishedAt ?? 0) >= dayStart)
  const active = rows.filter((d) => ACTIVE.has(d.status))
  const open = alerts('open').counts
  const health: HealthItem[] = [
    { key: 'database', status: 'ok', params: {}, details: { engine: 'localStorage (demo)' } },
    { key: 'gateway', status: 'ok', params: { domain: 'demo' }, details: { heartbeat_age_s: 0.4, uptime_s: Math.round((now - startedAt) / 1000), ros_domain: 'demo', nav_graph_path: NAV_GRAPH_PATH } },
    { key: 'rmf', status: 'ok', params: {}, details: { fleets_reporting: fleets().length, robots: robots.length } },
    { key: 'task_events', status: 'ok', params: {}, details: { enabled: true } },
    ...fleets().map((name): HealthItem => ({ key: 'fleet', status: 'ok',
      params: { name, online: online.filter((r) => r.fleet === name).length, total: robots.filter((r) => r.fleet === name).length },
      details: { state_age_s: 0.5, online: online.filter((r) => r.fleet === name).length, total: robots.filter((r) => r.fleet === name).length, adapter: 'demo_fleet_adapter', interface: 'AMR' } })),
    { key: 'adapter', status: 'ok', params: { name: fleets()[0] ?? 'demo', node: 'demo_fleet_adapter', state: 'ok' },
      details: { node: 'demo_fleet_adapter', reported_ago_s: 1, interval_s: 2, uptime_s: Math.round((now - startedAt) / 1000), robots_online: `${online.length}/${robots.length}`, messages_rx: Math.round((now - startedAt) / 100), messages_dropped: 0, publish_failed: 0 } },
    { key: 'mqtt', status: 'ok', params: { name: fleets()[0] ?? 'demo' }, details: { connected: true, connections_lost: 0 } },
  ]
  return {
    robots: {
      total: robots.length, online: online.length, offline: robots.length - online.length,
      moving: online.filter((r) => r.activity === 'moving').length, idle: online.filter((r) => r.activity === 'idle' && r.mode !== 'charging').length,
      charging: online.filter((r) => r.mode === 'charging').length, waiting: online.filter((r) => r.activity === 'waiting').length,
      paused: online.filter((r) => r.paused).length,
    },
    tasks: {
      active: active.length, queued: rows.filter((d) => d.status === 'queued').length, scheduled: rows.filter((d) => d.status === 'scheduled').length,
      completedToday: rows.filter((d) => d.status === 'completed').length, failedToday: rows.filter((d) => d.status === 'failed').length,
      cancelledToday: rows.filter((d) => d.status === 'cancelled').length,
      byKind: Object.fromEntries(['delivery', 'patrol'].map((k) => [k, active.filter((d) => d.kind === k).length])),
    },
    alerts: open,
    health,
  }
}

const TASK_GROUPS: Record<TaskGroup, (status: string) => boolean> = {
  active: (s) => ACTIVE.has(s as never),
  scheduled: (s) => s === 'scheduled',
  finished: (s) => FINAL.has(s as never),
  all: () => true,
}

const STATES: TaskState[] = ['SCHEDULED', 'QUEUED', 'ASSIGNED', 'EXECUTING', 'PAUSED', 'COMPLETED', 'CANCELLED', 'FAILED']

export function tasks(user: UserRow, group: string, service: string, q: string, state = '', mine = false): TaskListView {
  if (!(group in TASK_GROUPS)) throw new OperationError(422, 'tasks.invalid_group')
  const data = db()
  const needle = q.trim().toLowerCase().replace(/^#/, '')
  const rows = [...data.deliveries].filter((r) => taskVisible(user, r) && (!mine || r.requesterId === user.id)).sort((a, b) => b.createdAt - a.createdAt)
  const inService = rows.filter((r) => !service || r.service === service)
  const counts = Object.fromEntries(STATES.map((s) => [s, inService.filter((r) => taskState(r) === s).length])) as Record<TaskState, number>
  const groups = Object.fromEntries(Object.entries(TASK_GROUPS).map(([g, f]) => [g, inService.filter((r) => f(r.status)).length])) as Record<TaskGroup, number>
  const items = inService
    .filter((r) => TASK_GROUPS[group as TaskGroup](r.status) && (!state || taskState(r) === state))
    .map((r) => ({ ...toDelivery(data, r, user, true), rmfTaskId: `${r.kind}.dispatch-${r.id}` }))
    .filter((d) => !needle || [String(d.id), d.robot?.name ?? '', d.requester.fullName, d.service, d.pickup.name.vi, d.pickup.name.en,
      d.dropoff.name.vi, d.dropoff.name.en, d.area?.name.en ?? '', d.area?.name.vi ?? '', d.route?.name.en ?? '', d.route?.name.vi ?? '',
      ...d.stops.flatMap((s) => [s.name.vi, s.name.en])].some((h) => h.toLowerCase().includes(needle)))
  return { items, counts, groups, ...taskWatch(Date.now(), rows) }
}

const LATE_MS = 120_000

function taskWatch(now: number, rows = db().deliveries): Pick<FleetTaskList, 'summary' | 'unassigned' | 'late'> {
  const dayStart = new Date(now).setHours(0, 0, 0, 0)
  const ref = (r: (typeof rows)[number]): TaskRef => {
    const plan = r.plan
    const etaAt = plan ? Math.round(r.kind === 'patrol' ? plan.completeAt : plan.arriveAt) : null
    return { id: r.id, kind: r.kind, service: r.service, pickupId: r.pickupId, dropoffId: r.dropoffId, stops: r.stops, robot: r.robotName,
      since: Math.max(r.createdAt, r.scheduledAt ?? 0), etaAt }
  }
  const today = rows.filter((r) => (r.finishedAt ?? 0) >= dayStart)
  const done = today.filter((r) => r.status === 'completed').map((r) => ((r.finishedAt ?? 0) - Math.max(r.createdAt, r.scheduledAt ?? 0)) / 60000)
  const unassigned = rows.filter((r) => r.status === 'queued' && !r.robotName).map(ref).sort((a, b) => a.since - b.since)
  const late = rows.filter((r) => ACTIVE.has(r.status)).map(ref)
    .filter((r) => r.etaAt !== null && now - r.etaAt > LATE_MS)
    .map((r) => ({ ...r, lateMin: Math.round((now - (r.etaAt ?? now)) / 6000) / 10 }))
  return {
    summary: { active: rows.filter((r) => ACTIVE.has(r.status)).length, queued: unassigned.length, scheduled: rows.filter((r) => r.status === 'scheduled').length,
      completedToday: today.filter((r) => r.status === 'completed').length, failedToday: today.filter((r) => r.status === 'failed').length,
      avgDurationMin: done.length ? Math.round((done.reduce((a, b) => a + b, 0) / done.length) * 10) / 10 : null, late: late.length },
    unassigned,
    late,
  }
}

// Robot history of the demo: battery follows a daily cycle; active hours come from the demo tasks.

const SAMPLE_S = 60
const DAY_MS = 86_400_000

function demoBattery(name: string, at: number): number {
  const phase = [...name].reduce((a, c) => a + c.charCodeAt(0), 0)
  const cycle = Math.sin((at / 3_600_000 + phase) / 3)
  return Math.round((62 + 30 * cycle) * 10) / 10
}

function dayKey(ms: number): string {
  const d = new Date(ms)
  return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`
}

function utilizationDays(days: number, robot?: string): UtilizationDay[] {
  const now = Date.now()
  const start = new Date(now).setHours(0, 0, 0, 0) - (days - 1) * DAY_MS
  const out: UtilizationDay[] = []
  const robots = robot ? 1 : db().robots.length
  for (let d = 0; d < days; d++) {
    const from = start + d * DAY_MS
    const to = Math.min(from + DAY_MS, now)
    const span = Math.max(0, (to - from) / 3_600_000) * robots
    const active = db().deliveries
      .filter((r) => r.robotName && (!robot || r.robotName === robot) && r.finishedAt && r.finishedAt >= from && r.finishedAt < to)
      .reduce((h, r) => h + ((r.finishedAt ?? 0) - Math.max(r.createdAt, r.scheduledAt ?? 0)) / 3_600_000, 0)
    const charging = span * 0.15
    out.push({ day: dayKey(from), active: Math.round(active * 100) / 100, charging: Math.round(charging * 100) / 100,
      idle: Math.round(Math.max(0, span - active - charging) * 100) / 100, offline: 0 })
  }
  return out
}

export function robotDetail(name: string, hours: number): RobotDetailView {
  const now = Date.now()
  const robot = fleetRobots(now).find((r) => r.name === name)
  if (!robot) throw new OperationError(404, 'robot.not_found')
  const reg = registration().fleets.flatMap((f) => f.robots.map((r) => ({ ...r, fleet: f }))).find((r) => r.name === name)
  const vertices = siteLevels.find((l) => l.id === robot.levelId)?.graph.vertices.filter((v) => v.name) ?? []
  const nearest = vertices.reduce<{ name: string; d: number } | null>((best, v) => {
    const d = Math.hypot(v.x - robot.x, v.y - robot.y)
    return !best || d < best.d ? { name: v.name, d } : best
  }, null)
  const step = Math.max(SAMPLE_S * 1000, (hours * 3_600_000) / 240)
  const battery: [number, number][] = []
  for (let at = now - hours * 3_600_000; at < now; at += step) battery.push([Math.round(at), demoBattery(name, at)])
  battery.push([now, robot.battery])
  const rows = db().deliveries.filter((r) => r.robotName === name).sort((a, b) => b.createdAt - a.createdAt)
  const events: RobotEvent[] = rows.slice(0, 10).flatMap((r) => r.events.map((e) => ({ at: e.at, source: 'task' as const,
    level: e.type === 'failed' || e.type === 'delayed' ? 'warning' as const : 'info' as const, code: e.type, deliveryId: r.id })))
  const auditRows = (db().audit ?? []).filter((a) => a.target === name).map((a) => ({ at: a.at, source: 'audit' as const, level: 'info' as const,
    code: a.action, actor: userName(a.actorId), text: a.detail }))
  return {
    robot,
    technical: { rmf_mode: robot.mode, rmf_task: robot.taskId, nearest_waypoint: nearest?.name ?? null,
      nearest_distance_m: nearest ? Math.round(nearest.d * 100) / 100 : null, level: robot.levelId,
      position: `x ${robot.x.toFixed(2)} · y ${robot.y.toFixed(2)} · ${Math.round((robot.yaw * 180) / Math.PI)}°`,
      adapter: robot.adapter, controls: robot.controls, paused: robot.paused, speed_limit_mps: robot.speedLimit,
      fleet_state_age_s: 0.5, interface: reg?.fleet.interface ?? null, manufacturer: reg?.manufacturer ?? null,
      serial: reg?.serial ?? null, charger: reg?.charger ?? null, path_points: 0 },
    tasks: rows.slice(0, 10).map((r) => ({ id: r.id, kind: r.kind, status: r.status, pickupId: r.pickupId, dropoffId: r.dropoffId,
      createdAt: r.createdAt, finishedAt: r.finishedAt })),
    events: [...events, ...auditRows].sort((a, b) => b.at - a.at).slice(0, 60),
    battery,
    hours,
    utilization: utilizationDays(7, name),
    sampleS: SAMPLE_S,
  }
}

export function analytics(days: number, user?: UserRow, service = '', robot = '', zone = '', services: string[] = []): AnalyticsReport {
  if (![1, 7, 30].includes(days)) throw new OperationError(422, 'analytics.invalid_range')
  const now = Date.now()
  const since = new Date(now).setHours(0, 0, 0, 0) - (days - 1) * DAY_MS
  const rows = db().deliveries.filter((r) => (!user || taskVisible(user, r)) && (!service || r.service === service)
    && (!robot || r.robotName === robot) && (!zone || r.zones.includes(zone)))
  const created = rows.filter((r) => r.createdAt >= since)
  const finished = rows.filter((r) => (r.finishedAt ?? 0) >= since)
  const keys = Array.from({ length: days }, (_, i) => dayKey(since + i * DAY_MS))
  const tasksPerDay = keys.map((day) => ({ day, created: created.filter((r) => dayKey(r.createdAt) === day).length,
    completed: finished.filter((r) => r.status === 'completed' && dayKey(r.finishedAt ?? 0) === day).length,
    failed: finished.filter((r) => r.status === 'failed' && dayKey(r.finishedAt ?? 0) === day).length,
    cancelled: finished.filter((r) => r.status === 'cancelled' && dayKey(r.finishedAt ?? 0) === day).length }))
  const count = (s: string) => finished.filter((r) => r.status === s).length
  const decided = count('completed') + count('failed')
  const minutes = (values: number[]) => (values.length ? Math.round((values.reduce((a, b) => a + b, 0) / values.length) * 10) / 10 : null)
  const durations = finished.filter((r) => r.status === 'completed').map((r) => ((r.finishedAt ?? 0) - Math.max(r.createdAt, r.scheduledAt ?? 0)) / 60000)
  const waits = finished.flatMap((r) => r.events.filter((e) => e.type === 'assigned').map((e) => Math.max(0, e.at - Math.max(r.createdAt, r.scheduledAt ?? 0)) / 60000))
  const places = new Map<string, number>()
  created.forEach((r) => (r.kind === 'delivery' ? [r.dropoffId] : r.stops).forEach((id) => places.set(id, (places.get(id) ?? 0) + 1)))
  const locations = db().locations
  const utilization = utilizationDays(days)
  const busy = utilization.reduce((h, d) => h + d.active, 0)
  const available = utilization.reduce((h, d) => h + d.active + d.idle + d.charging, 0)
  const robots = fleetRobots(now).filter((r) => !robot || r.name === robot)
  const open = alerts('all').items.filter((a) => a.openedAt >= since)
  return {
    days,
    kpis: { created: created.length, completed: count('completed'), failed: count('failed'), cancelled: count('cancelled'),
      successRate: decided ? Math.round((1000 * count('completed')) / decided) / 10 : null,
      avgDurationMin: minutes(durations), avgWaitMin: minutes(waits),
      utilizationPct: available ? Math.round((1000 * busy) / available) / 10 : null, alerts: open.length },
    tasksPerDay,
    topPlaces: [...places.entries()].sort((a, b) => b[1] - a[1]).slice(0, 8)
      .map(([id, n]) => ({ id, name: locations.find((l) => l.id === id)?.name ?? { vi: id, en: id }, count: n })),
    utilization,
    battery: [[0, 20], [20, 40], [40, 60], [60, 80], [80, 101]].map(([lo, hi]) => ({ from: lo, to: Math.min(hi, 100), robots: robots.filter((r) => r.battery >= lo && r.battery < hi).length })),
    alertsPerDay: keys.map((day) => ({ day, critical: open.filter((a) => a.severity === 'critical' && dayKey(a.openedAt) === day).length,
      warning: open.filter((a) => a.severity === 'warning' && dayKey(a.openedAt) === day).length,
      info: open.filter((a) => a.severity === 'info' && dayKey(a.openedAt) === day).length })),
    alertCodes: [...open.reduce((m, a) => m.set(a.code, (m.get(a.code) ?? 0) + 1), new Map<string, number>())].map(([code, n]) => ({ code, count: n })),
    sampleS: SAMPLE_S,
    byService: SERVICES.filter((sv) => services.includes(sv.id) && (!service || sv.id === service)).map((sv) => {
      const own = finished.filter((r) => r.service === sv.id && r.status === 'completed')
      const durations = own.map((r) => ((r.finishedAt ?? 0) - (r.startedAt ?? r.createdAt)) / 60000)
      const avg = minutes(durations)
      const base = { id: sv.id, category: sv.category, created: created.filter((r) => r.service === sv.id).length, completed: own.length, avgDurationMin: avg }
      if (sv.category === 'delivery') return { ...base, distanceKm: null }
      if (sv.category === 'clean') return { ...base, areaM2: null, coveragePct: null, areas: new Set(own.map((r) => r.params.area)).size }
      return { ...base, rounds: finished.filter((r) => r.service === sv.id).reduce((n, r) => n + r.roundsDone, 0),
        zonesCovered: new Set(own.flatMap((r) => r.zones)).size, avgRoundMin: minutes(own.map((r, i) => durations[i] / Math.max(1, r.rounds))) }
    }),
  }
}

export function cancelTask(user: UserRow, id: number) {
  const data = db()
  const row = data.deliveries.find((d) => d.id === id)
  if (!row || !taskVisible(user, row)) throw new OperationError(404, 'delivery.not_found')
  if (!toDelivery(data, row, user, true).actions.cancel) throw new OperationError(409, 'delivery.not_cancellable')
  cancel(row, Date.now())
  markChanged('fleet')
  return { ...toDelivery(data, row, user, true), rmfTaskId: null }
}

// Administration

const AUDIT_KEEP = 300

export function audit(user: UserRow | null, action: string, target: string, detail: unknown = ''): void {
  const data = db()
  const rows = data.audit ?? []
  data.audit = [{ id: (rows[0]?.id ?? 0) + 1, at: Date.now(), actorId: user?.id ?? null, action, target,
    detail: typeof detail === 'string' ? detail : JSON.stringify(detail) }, ...rows].slice(0, AUDIT_KEEP)
  save()
}

export function auditList(q: string, action: string, limit = 200): AuditList {
  const needle = q.trim().toLowerCase()
  const auditRows = db().audit ?? []
  const items = auditRows
    .filter((r) => !action || r.action.startsWith(action))
    .map((r) => ({ id: r.id, at: r.at, actor: userName(r.actorId), action: r.action, target: r.target, detail: r.detail }))
    .filter((e) => !needle || Object.values(e).some((v) => v !== null && String(v).toLowerCase().includes(needle)))
  return { items: items.slice(0, limit), actions: [...new Set(auditRows.map((r) => r.action))].sort() }
}

export function adminUser(row: UserRow): AdminUser {
  const admin = row.role === 'admin'
  return { id: row.id, email: row.email, fullName: row.fullName, role: row.role, department: row.department, active: row.active, locale: row.locale,
    allowedServices: admin ? [] : row.allowedServices, allowedZones: admin ? [] : row.allowedZones, permissions: permissionsOf(row),
    createdAt: row.createdAt ?? startedAt, lastLoginAt: row.lastLoginAt ?? null, lastActiveAt: row.lastActiveAt ?? row.lastLoginAt ?? null }
}

function accessFields(body: Record<string, unknown>): Partial<Pick<UserRow, 'allowedServices' | 'allowedZones' | 'permissions'>> {
  const out: Partial<Pick<UserRow, 'allowedServices' | 'allowedZones' | 'permissions'>> = {}
  const ids = (v: unknown, known: string[], code: string) => {
    if (!Array.isArray(v) || !v.every((x) => known.includes(String(x)))) throw new OperationError(422, code)
    return [...new Set(v.map(String))]
  }
  if ('allowedServices' in body) out.allowedServices = ids(body.allowedServices, SERVICES.map((s) => s.id), 'users.invalid_service')
  if ('allowedZones' in body) out.allowedZones = ids(body.allowedZones, db().zones.map((z) => z.id), 'users.invalid_zone')
  if ('permissions' in body) out.permissions = ids(body.permissions, GRANTABLE, 'users.invalid_permission') as UserRow['permissions']
  return out
}

export function createUser(actor: UserRow, body: Record<string, unknown>, minPassword: number): AdminUser {
  const email = String(body.email ?? '').trim().toLowerCase()
  const fullName = String(body.fullName ?? '').trim()
  const role = body.role ?? 'operator'
  if (!/^[^@\s]+@[^@\s]+\.[^@\s]+$/.test(email) || !fullName || (role !== 'operator' && role !== 'admin')) throw new OperationError(422, 'users.invalid')
  if (String(body.password ?? '').length < minPassword) throw new OperationError(422, 'auth.password_too_short')
  const data = db()
  if (data.users.some((u) => u.email === email)) throw new OperationError(409, 'users.email_taken')
  const access = accessFields(body)
  const row: UserRow = { id: `u-${Date.now()}`, email, password: String(body.password), fullName, role, locale: 'vi',
    department: String(body.department ?? ''), allowedServices: access.allowedServices ?? [], allowedZones: access.allowedZones ?? [],
    permissions: access.permissions ?? [...DEFAULT_OPERATOR],
    notificationPrefs: { deliveryUpdates: true, delays: true }, active: body.active !== false, createdAt: Date.now(), lastLoginAt: null }
  data.users.push(row)
  save()
  audit(actor, 'user.create', email, `role=${role}`)
  return adminUser(row)
}

export function updateUser(actor: UserRow, id: string, body: Record<string, unknown>): AdminUser {
  const row = db().users.find((u) => u.id === id)
  if (!row) throw new OperationError(404, 'users.not_found')
  const changes: string[] = []
  if (typeof body.fullName === 'string' && body.fullName.trim() && body.fullName.trim() !== row.fullName) {
    changes.push(`name: ${row.fullName} → ${body.fullName.trim()}`)
    row.fullName = body.fullName.trim()
  }
  if (typeof body.department === 'string' && body.department.trim() !== row.department) {
    changes.push(`department: ${row.department || '-'} → ${body.department.trim() || '-'}`)
    row.department = body.department.trim()
  }
  for (const [key, value] of Object.entries(accessFields(body)) as [keyof ReturnType<typeof accessFields>, string[]][]) {
    if ([...value].sort().join() !== [...row[key]].sort().join()) {
      changes.push(`${key.replace('allowed', '').toLowerCase()}: ${value.join(', ') || '-'}`)
      ;(row as unknown as Record<string, string[]>)[key] = value
    }
  }
  if ((body.role === 'operator' || body.role === 'admin') && body.role !== row.role) {
    if (row.id === actor.id) throw new OperationError(409, 'users.self')
    changes.push(`role: ${row.role} → ${body.role}`)
    row.role = body.role
  }
  if (typeof body.active === 'boolean' && body.active !== row.active) {
    if (row.id === actor.id) throw new OperationError(409, 'users.self')
    row.active = body.active
    changes.push(body.active ? 'enabled' : 'disabled')
  }
  save()
  if (changes.length) audit(actor, 'user.update', row.email, changes.join('; '))
  return adminUser(row)
}

export function setPassword(actor: UserRow, id: string, password: unknown, minPassword: number): void {
  const row = db().users.find((u) => u.id === id)
  if (!row) throw new OperationError(404, 'users.not_found')
  if (typeof password !== 'string' || password.length < minPassword) throw new OperationError(422, 'auth.password_too_short')
  row.password = password
  save()
  audit(actor, 'user.password', row.email)
}

export function infrastructure(): Infrastructure {
  const used = new Map(registration().fleets.flatMap((f) => f.chargers.map((c) => [c.name, c.used_by ?? null] as const)))
  return {
    source: NAV_GRAPH_PATH,
    chargers: siteLevels.flatMap((l) => l.graph.vertices.filter((v) => v.charger).map((v) => ({ name: v.name, level: l.id, x: v.x, y: v.y, usedBy: used.get(v.name) ?? null }))),
    doors: [],
    lifts: [],
  }
}

export function adminOverview(now: number): AdminOverview {
  const base = overview(now)
  const fleetHealth = base.health.filter((h) => h.key === 'fleet')
  const integrations = base.health.filter((h) => ['gateway', 'rmf', 'task_events', 'adapter', 'mqtt'].includes(h.key))
  const users = db().users.filter((u) => u.active)
  return {
    ...base,
    fleets: { configured: fleetHealth.length, healthy: fleetHealth.filter((h) => h.status === 'ok').length },
    maps: { levels: siteLevels.length, editable: true, navGraphPath: NAV_GRAPH_PATH },
    integrations: { total: integrations.length, healthy: integrations.filter((h) => h.status === 'ok').length },
    users: { active: users.length, admins: users.filter((u) => u.role === 'admin').length, operators: users.filter((u) => u.role === 'operator').length },
    recentChanges: auditList('', '', 8).items,
  }
}
