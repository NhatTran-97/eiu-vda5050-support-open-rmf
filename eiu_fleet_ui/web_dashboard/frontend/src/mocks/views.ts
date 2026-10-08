// Turn demo database rows into the DTOs of the API contract.
import type {
  AppNotification,
  Delivery,
  DeliveryGroup,
  DeliveryStatus,
  Level,
  Location,
  LocationRef,
  TaskKind,
  TaskState,
  TimelineKey,
  TimelineStep,
} from '../api/types'
import { can } from './access'
import type { DbData, DeliveryRow, LocationRow, NotificationRow, UserRow } from './schema'
import { siteLevels, waypointPoint } from './site'

export const ACTIVE: ReadonlySet<DeliveryStatus> = new Set(['queued', 'to_pickup', 'at_pickup', 'in_transit', 'arrived', 'underway'])
const CANCELLABLE: Record<TaskKind, ReadonlySet<DeliveryStatus>> = {
  delivery: new Set(['scheduled', 'queued', 'to_pickup', 'at_pickup']),
  patrol: new Set(['scheduled', 'queued', 'underway']),
  clean: new Set(['scheduled', 'queued', 'underway']),
}
export const FINAL: ReadonlySet<DeliveryStatus> = new Set(['completed', 'cancelled', 'failed'])

export { can, toMe } from './access'

export function taskState(row: DeliveryRow): TaskState {
  if (FINAL.has(row.status)) return row.status.toUpperCase() as TaskState
  if (row.status === 'scheduled') return 'SCHEDULED'
  if (row.paused) return 'PAUSED'
  if (row.status === 'queued') return row.robotName ? 'ASSIGNED' : 'QUEUED'
  return 'EXECUTING'
}

export function toLevels(data: DbData): Level[] {
  return siteLevels.map((l) => ({
    id: l.id,
    label: data.levelLabels[l.id] ?? { vi: l.id, en: l.id },
    imageUrl: '/' + l.image,
    origin: l.origin,
    resolution: l.resolution,
    widthPx: l.widthPx,
    heightPx: l.heightPx,
  }))
}

export function toLocation(row: LocationRow): Location {
  const p = waypointPoint(row.levelId, row.waypoint) ?? { x: 0, y: 0 }
  return { ...row, x: p.x, y: p.y }
}

function ref(data: DbData, id: string): LocationRef {
  const row = data.locations.find((l) => l.id === id)
  return row
    ? { id: row.id, name: row.name, category: row.category, levelId: row.levelId }
    : { id, name: { vi: id, en: id }, category: 'room', levelId: '' }
}

export function groupOf(status: DeliveryStatus): DeliveryGroup {
  if (status === 'scheduled') return 'upcoming'
  return ACTIVE.has(status) ? 'active' : 'completed'
}

const STEP_DONE_BY: Record<TimelineKey, string> = {
  requested: 'requested',
  picked_up: 'picked_up',
  en_route: 'arriving',
  arriving: 'arrived',
  delivered: 'completed',
  assigned: 'assigned',
  patrolling: 'completed',
  cleaning: 'completed',
}

const STEP_STARTED_BY: Record<TimelineKey, string | null> = {
  requested: null,
  picked_up: 'assigned',
  en_route: 'picked_up',
  arriving: 'arriving',
  delivered: 'arrived',
  assigned: null,
  patrolling: 'assigned',
  cleaning: 'assigned',
}

const STEPS: Record<TaskKind, TimelineKey[]> = {
  delivery: ['requested', 'picked_up', 'en_route', 'arriving', 'delivered'],
  patrol: ['requested', 'assigned', 'patrolling'],
  clean: ['requested', 'assigned', 'cleaning'],
}

/** Five user-facing steps; `at` is when a done step finished or when the current step began. */
function timeline(row: DeliveryRow): TimelineStep[] {
  const eventAt = (type: string | null) => (type ? row.events.find((e) => e.type === type)?.at ?? null : null)
  const keys = STEPS[row.kind]
  let currentTaken = false
  return keys.map((key) => {
    const doneAt = key === 'requested' ? row.createdAt : eventAt(STEP_DONE_BY[key])
    if (doneAt !== null) return { key, state: 'done', at: doneAt }
    if (row.status === 'cancelled' || row.status === 'failed') return { key, state: 'skipped', at: null }
    if (!currentTaken && ACTIVE.has(row.status)) {
      currentTaken = true
      return { key, state: 'current', at: eventAt(STEP_STARTED_BY[key]) }
    }
    return { key, state: 'pending', at: null }
  })
}

/** Task DTO; the caller checked that the viewer may see the task. */
export function toDelivery(data: DbData, row: DeliveryRow, viewer: UserRow, _operator = false): Delivery {
  const requester = data.users.find((u) => u.id === row.requesterId)
  const own = row.requesterId === viewer.id
  const robot = row.robotName ? data.robots.find((r) => r.name === row.robotName) : undefined
  const state = taskState(row)
  const area = row.kind === 'clean' ? data.areas.find((a) => a.id === row.params.area) : undefined
  const route = row.params.route ? data.routes.find((r) => r.id === row.params.route) : undefined
  const moving = row.status === 'to_pickup' || row.status === 'at_pickup' || row.status === 'in_transit' || row.status === 'underway'
  return {
    id: row.id,
    kind: row.kind,
    service: row.service,
    state,
    priority: row.priority,
    parameters: row.params,
    zones: row.zones,
    area: area ? { id: area.id, name: area.name, zone: area.zone } : null,
    route: route ? { id: route.id, name: route.name, zone: route.zone } : null,
    repeat: row.repeat,
    previousId: row.previousId,
    requestedRobot: row.requestedRobot,
    startedAt: row.startedAt,
    status: row.status,
    group: groupOf(row.status),
    pickup: ref(data, row.pickupId),
    dropoff: ref(data, row.dropoffId),
    stops: row.stops.map((id) => ref(data, id)),
    rounds: row.rounds,
    roundsDone: row.roundsDone,
    packageType: row.packageType,
    note: row.note,
    requester: { id: row.requesterId, fullName: requester?.fullName ?? '' },
    robot: row.robotName ? { name: row.robotName, fleet: robot?.fleet ?? '' } : null,
    createdAt: row.createdAt,
    scheduledAt: row.scheduledAt,
    finishedAt: row.finishedAt,
    etaAt: moving && row.plan ? Math.round(row.kind === 'patrol' ? row.plan.completeAt : row.plan.arriveAt) : null,
    error: null,
    timeline: timeline(row),
    actions: {
      cancel: CANCELLABLE[row.kind].has(row.status) && can(viewer, 'task.cancel'),
      pause: state === 'EXECUTING' && can(viewer, 'task.pause'),
      resume: state === 'PAUSED' && can(viewer, 'task.pause'),
      reassign: ['SCHEDULED', 'QUEUED', 'ASSIGNED'].includes(state) && can(viewer, 'task.reassign'),
      track: ACTIVE.has(row.status),
      reorder: own && FINAL.has(row.status) && can(viewer, 'task.create'),
    },
  }
}

export function toNotification(row: NotificationRow): AppNotification {
  return {
    id: row.id,
    type: row.type,
    deliveryId: row.deliveryId,
    params: row.params,
    severity: row.type === 'task_failed' ? 'critical' : ['delayed', 'action_required', 'cancelled'].includes(row.type) ? 'warning' : 'info',
    createdAt: row.createdAt,
    readAt: row.readAt,
  }
}
