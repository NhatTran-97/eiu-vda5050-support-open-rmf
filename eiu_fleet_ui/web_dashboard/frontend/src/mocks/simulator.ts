// Demo fleet: assigns queued deliveries to idle robots and advances them along their plans.
import type { NotificationType, RealtimeTopic, RobotLive } from '../api/types'
import { db, save } from './db'
import { batteryAt, milestones, poseAt, stageAt, type Milestone } from './plan'
import type { DeliveryRow, RobotRow } from './schema'
import { demoSettings } from './settings'
import { fleetCapabilities, serviceOf } from './access'
import { levelGraph, planDelivery, planPatrol, vertexIndex, waypointPoint } from './site'
import type { Plan } from './plan'
import { ACTIVE } from './views'

export interface Frame {
  robots: RobotLive[]
  topics: RealtimeTopic[]
  periodMs: number
}

type Listener = (frame: Frame) => void

const listeners = new Set<Listener>()
const pending = new Set<RealtimeTopic>()
let timer: number | undefined

const MILESTONE_NOTIFICATION: Record<Milestone, NotificationType> = {
  arrived_pickup: 'robot_at_pickup',
  picked_up: 'package_loaded',
  arriving: 'near_destination',
  arrived: 'robot_arrived',
  completed: 'delivered',
}

const PROGRESS_TYPES: ReadonlySet<NotificationType> = new Set(['robot_at_pickup', 'package_loaded', 'near_destination'])

export function markChanged(...topics: RealtimeTopic[]): void {
  topics.forEach((t) => pending.add(t))
  // Operators follow every task.
  if (topics.includes('deliveries')) pending.add('fleet')
}

export function notify(row: DeliveryRow, type: NotificationType, at: number): void {
  const data = db()
  const user = data.users.find((u) => u.id === row.requesterId)
  if (!user) return
  if (PROGRESS_TYPES.has(type) && !user.notificationPrefs.deliveryUpdates) return
  if (type === 'delayed' && !user.notificationPrefs.delays) return
  const name = (id: string) => data.locations.find((l) => l.id === id)?.name ?? { vi: id, en: id }
  data.notifications.push({
    id: data.nextNotificationId++,
    userId: user.id,
    type,
    deliveryId: row.id,
    params: { id: row.id, robot: row.robotName ?? '', pickup: name(row.pickupId), dropoff: name(row.dropoffId), rounds: row.rounds },
    createdAt: Math.round(at),
    readAt: null,
  })
  markChanged('notifications')
}

function robotOf(row: DeliveryRow): RobotRow | undefined {
  return db().robots.find((r) => r.name === row.robotName)
}

function release(robot: RobotRow | undefined, row: DeliveryRow, at: number): void {
  if (!robot || !row.plan) return
  const pose = poseAt(row.plan, at)
  robot.x = pose.x
  robot.y = pose.y
  robot.yaw = pose.yaw
  robot.battery = batteryAt(row.plan, at, demoSettings.batteryDrainPerM)
  robot.deliveryId = null
}

/** Waypoints a task visits after its first one: the stops of a patrol, a short loop around the area of a cleaning. */
function cleaningLoop(levelId: string, waypoint: string): string[] {
  const graph = levelGraph(levelId)
  const index = vertexIndex(levelId, waypoint)
  const lane = graph?.lanes.find(([a, b]) => (a === index || b === index) && graph.vertices[a === index ? b : a]?.name)
  const other = lane && graph ? graph.vertices[lane[0] === index ? lane[1] : lane[0]].name : null
  return other ? [waypoint, other, waypoint] : [waypoint]
}

/** Robots in a maintenance window or item in progress take no task. */
export function inMaintenance(robot: string, now: number): boolean {
  return db().maintenance.some((m) => m.robot === robot && (m.status === 'in_progress'
    || (m.status === 'planned' && m.windowStart !== null && m.windowStart <= now && now < (m.windowEnd ?? m.windowStart))))
}

function assign(row: DeliveryRow, now: number): boolean {
  const data = db()
  const levelId = data.locations[0]?.levelId ?? ''
  const caps = serviceOf(row.service)?.requiredCapabilities ?? []
  const pickup = data.locations.find((l) => l.id === row.pickupId)
  const dropoff = data.locations.find((l) => l.id === row.dropoffId)
  const area = row.kind === 'clean' ? data.areas.find((a) => a.id === row.params.area) : undefined
  const firstWp = row.kind === 'clean' ? area?.rmfZone : pickup?.waypoint
  const target = firstWp ? waypointPoint(levelId, firstWp) : null
  if (!target || (row.kind === 'delivery' && !dropoff)) {
    fail(row, now)
    return true
  }
  const idle = data.robots.filter((r) => r.deliveryId === null && r.levelId === levelId && !inMaintenance(r.name, now)
    && caps.every((c) => fleetCapabilities(r.fleet).includes(c)) && (!row.requestedRobot || r.name === row.requestedRobot))
  if (idle.length === 0) return false
  idle.sort((a, b) => Math.hypot(a.x - target.x, a.y - target.y) - Math.hypot(b.x - target.x, b.y - target.y))
  const robot = idle[0]
  let plan: Plan | null
  if (row.kind === 'delivery') plan = planDelivery(levelId, robot, pickup!.waypoint, dropoff!.waypoint, now, robot.battery)
  else if (row.kind === 'clean') plan = planPatrol(levelId, robot, cleaningLoop(levelId, firstWp!), 2, now, robot.battery)
  else plan = planPatrol(levelId, robot, row.stops.map((id) => data.locations.find((l) => l.id === id)?.waypoint ?? ''), row.rounds, now, robot.battery)
  if (!plan) {
    fail(row, now)
    return true
  }
  robot.deliveryId = row.id
  row.robotName = robot.name
  row.plan = plan
  row.status = row.kind === 'delivery' ? 'to_pickup' : 'underway'
  row.startedAt = now
  row.events.push({ type: 'assigned', at: now, detail: robot.name })
  if (row.kind === 'patrol') notify(row, 'patrol_started', now)
  return true
}

/** Next task of a repeating series, once the current one is due. */
function nextOccurrence(row: DeliveryRow, now: number): void {
  const data = db()
  if (row.repeat === 'none' || row.scheduledAt === null || data.deliveries.some((d) => d.previousId === row.id)) return
  const step = (at: number) => {
    const d = new Date(at)
    d.setDate(d.getDate() + (row.repeat === 'weekly' ? 7 : 1))
    while (row.repeat === 'weekdays' && (d.getDay() === 0 || d.getDay() === 6)) d.setDate(d.getDate() + 1)
    return d.getTime()
  }
  let at = step(row.scheduledAt)
  while (at < now + 60_000) at = step(at)
  data.deliveries.push({
    ...row, id: data.nextDeliveryId++, previousId: row.id, createdAt: now, scheduledAt: at, status: 'scheduled', robotName: null, plan: null,
    startedAt: null, finishedAt: null, roundsDone: 0, paused: false, events: [{ type: 'requested', at: now, detail: `repeat of #${row.id}` }],
  })
}

/** Pause freezes the robot where it is; resume shifts the rest of the plan by the time it stood still. */
export function setPaused(row: DeliveryRow, paused: boolean, now: number): void {
  if (!row.plan || row.paused === paused) return
  if (paused) {
    row.pausedAt = now
  } else {
    const dt = now - (row.pausedAt ?? now)
    const p = row.plan
    row.plan = { ...p, startAt: p.startAt + dt, pickupArriveAt: p.pickupArriveAt + dt, departAt: p.departAt + dt, arrivingAt: p.arrivingAt + dt,
      arriveAt: p.arriveAt + dt, completeAt: p.completeAt + dt }
    row.pausedAt = null
  }
  row.paused = paused
  row.events.push({ type: paused ? 'paused' : 'resumed', at: now })
  markChanged('deliveries')
  save()
}

function fail(row: DeliveryRow, now: number): void {
  row.status = 'failed'
  row.finishedAt = now
  row.events.push({ type: 'failed', at: now })
}

/** Bring the demo fleet up to `now`; events that fell between ticks keep their own times. */
export function tick(now: number): void {
  const data = db()
  let changed = false

  for (const row of data.deliveries) {
    if (row.status === 'scheduled' && row.scheduledAt !== null && row.scheduledAt <= now) {
      row.status = 'queued'
      nextOccurrence(row, now)
      changed = true
    }
  }

  const queued = data.deliveries
    .filter((r) => r.status === 'queued')
    .sort((a, b) => (a.scheduledAt ?? a.createdAt) - (b.scheduledAt ?? b.createdAt))
  for (const row of queued) {
    const done = assign(row, now)
    changed ||= done
    if (!done) break
  }

  for (const row of data.deliveries) {
    if (!row.plan || !ACTIVE.has(row.status) || row.kind === 'delivery' || row.paused) continue
    const done = Math.min(row.rounds, Math.floor(poseAt(row.plan, now).traveledM
      / Math.max(1e-6, poseAt(row.plan, row.plan.completeAt).traveledM) * row.rounds))
    if (done !== row.roundsDone) {
      row.roundsDone = done
      changed = true
    }
    if (now >= row.plan.completeAt) {
      row.status = 'completed'
      row.roundsDone = row.rounds
      row.finishedAt = Math.round(row.plan.completeAt)
      row.events.push({ type: 'completed', at: row.finishedAt })
      if (row.kind === 'patrol') notify(row, 'patrol_completed', row.plan.completeAt)
      release(robotOf(row), row, row.plan.completeAt)
      changed = true
    }
  }

  for (const row of data.deliveries) {
    if (!row.plan || !ACTIVE.has(row.status) || row.kind !== 'delivery' || row.paused) continue
    for (const [milestone, at] of milestones(row.plan)) {
      if (at > now) break
      if (row.events.some((e) => e.type === milestone)) continue
      row.events.push({ type: milestone, at: Math.round(at) })
      notify(row, MILESTONE_NOTIFICATION[milestone], at)
      changed = true
    }
    const stage = stageAt(row.plan, now)
    if (stage !== row.status) {
      row.status = stage
      changed = true
    }
    if (stage === 'completed') {
      row.finishedAt = Math.round(row.plan.completeAt)
      release(robotOf(row), row, row.plan.completeAt)
    }
  }

  if (changed) {
    markChanged('deliveries')
    save()
  }
}

export function cancel(row: DeliveryRow, now: number): void {
  release(robotOf(row), row, now)
  row.status = 'cancelled'
  row.finishedAt = now
  row.events.push({ type: 'cancelled', at: now })
  notify(row, 'cancelled', now)
  markChanged('deliveries')
  save()
}

const round = (v: number, digits: number) => Number(v.toFixed(digits))

export function liveRobots(now: number): RobotLive[] {
  const data = db()
  return data.robots.map((robot) => {
    const row = robot.deliveryId !== null ? data.deliveries.find((d) => d.id === robot.deliveryId) : undefined
    if (row?.plan) {
      const at = row.paused ? row.pausedAt ?? now : now
      const pose = poseAt(row.plan, at)
      return {
        name: robot.name, fleet: robot.fleet, levelId: robot.levelId,
        x: round(pose.x, 3), y: round(pose.y, 3), yaw: round(pose.yaw, 3),
        battery: round(batteryAt(row.plan, at, demoSettings.batteryDrainPerM), 1),
        activity: pose.moving && !row.paused ? 'moving' : 'waiting',
        deliveryId: row.id,
        remainingM: round(pose.remainingM, 1),
        path: pose.path.map((p) => [round(p.x, 3), round(p.y, 3)] as [number, number]),
      }
    }
    return {
      name: robot.name, fleet: robot.fleet, levelId: robot.levelId,
      x: round(robot.x, 3), y: round(robot.y, 3), yaw: round(robot.yaw, 3), battery: round(robot.battery, 1),
      activity: 'idle', deliveryId: null, remainingM: null, path: [],
    }
  })
}

export function subscribe(listener: Listener): () => void {
  listeners.add(listener)
  return () => listeners.delete(listener)
}

export function startSimulator(): void {
  if (timer !== undefined) return
  const step = () => {
    const now = Date.now()
    tick(now)
    const frame: Frame = { robots: liveRobots(now), topics: [...pending], periodMs: demoSettings.tickMs }
    pending.clear()
    listeners.forEach((l) => l(frame))
  }
  step()
  timer = window.setInterval(step, demoSettings.tickMs)
}
