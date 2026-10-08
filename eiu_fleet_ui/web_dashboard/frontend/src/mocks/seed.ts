// Demo data, generated relative to the current time so the pages always show recent activity.
import type { Area, DeliveryTemplate, I18nText, MaintenanceItem, PackageType, PatrolRoute } from '../api/types'
import { DEFAULT_OPERATOR } from './permissions'
import type { DbData, DeliveryRow, LocationRow, NotificationRow, RobotRow, UserRow, ZoneRow } from './schema'
import { planDelivery, siteLevels, waypointPoint } from './site'

export const DB_VERSION = 4

const MIN = 60_000
const HOUR = 60 * MIN
const DAY = 24 * HOUR

const prefs = { deliveryUpdates: true, delays: true }

/** Admin, and the three operators of the product brief: A (delivery + cleaning), B (delivery), C (patrol). */
const users: UserRow[] = [
  { id: 'u-admin', email: 'admin@eiu.edu.vn', password: 'admin1234', fullName: 'Trần Quốc Bảo', role: 'admin', department: 'IT Services',
    allowedServices: [], allowedZones: [], permissions: [], locale: 'vi', notificationPrefs: prefs, active: true },
  { id: 'u-nhat', email: 'nhat.tran@eiu.edu.vn', password: 'demo1234', fullName: 'Trần Duy Nhất', role: 'operator', department: 'EIU Robotics',
    allowedServices: ['delivery', 'cleaning'], allowedZones: [], permissions: [...DEFAULT_OPERATOR, 'task.pause'], locale: 'vi', notificationPrefs: prefs, active: true },
  { id: 'u-op-a', email: 'operator.a@eiu.edu.vn', password: 'demo1234', fullName: 'Nguyễn Minh Anh', role: 'operator', department: 'FabLab',
    allowedServices: ['delivery', 'cleaning'], allowedZones: ['building_a', 'building_b'], permissions: [...DEFAULT_OPERATOR, 'task.pause'],
    locale: 'vi', notificationPrefs: prefs, active: true },
  { id: 'u-op-b', email: 'operator.b@eiu.edu.vn', password: 'demo1234', fullName: 'Lê Thu Hà', role: 'operator', department: 'Library',
    allowedServices: ['delivery'], allowedZones: ['building_a'], permissions: [...DEFAULT_OPERATOR], locale: 'vi', notificationPrefs: prefs, active: true },
  { id: 'u-op-c', email: 'operator.c@eiu.edu.vn', password: 'demo1234', fullName: 'Phạm Đức Long', role: 'operator', department: 'Campus Security',
    allowedServices: ['patrol'], allowedZones: [], permissions: [...DEFAULT_OPERATOR], locale: 'en', notificationPrefs: prefs, active: true },
]

const firstLevel = siteLevels[0].id
const buildingA: I18nText = { vi: 'Tòa nhà A', en: 'Building A' }
const buildingB: I18nText = { vi: 'Tòa nhà B', en: 'Building B' }

const zones: ZoneRow[] = [
  { id: 'building_a', name: buildingA, levelId: firstLevel, enabled: true },
  { id: 'building_b', name: buildingB, levelId: firstLevel, enabled: true },
]

const locations: LocationRow[] = [
  { id: 'fablab', name: { vi: 'FABLAB (Tầng dưới)', en: 'FABLAB (Lower Level)' }, category: 'lab', waypoint: 'Patrol_A2', zone: 'building_a' },
  { id: 'library', name: { vi: 'Thư viện', en: 'Library' }, category: 'library', waypoint: 'Patrol_B1', zone: 'building_a' },
  { id: 'student_center', name: { vi: 'Trung tâm Sinh viên', en: 'Student Center' }, category: 'center', waypoint: 'Patrol_C1', zone: 'building_a' },
  { id: 'cafeteria', name: { vi: 'Căng tin', en: 'Cafeteria' }, category: 'cafeteria', waypoint: 'Patrol_E1', zone: 'building_a' },
  { id: 'room_204', name: { vi: 'Phòng 204', en: 'Room 204' }, category: 'room', waypoint: 'Patrol_F3', zone: 'building_b' },
  { id: 'room_118', name: { vi: 'Phòng 118', en: 'Room 118' }, category: 'room', waypoint: 'Patrol_D3', zone: 'building_b' },
  { id: 'room_312', name: { vi: 'Phòng 312', en: 'Room 312' }, category: 'room', waypoint: 'Patrol_C3', zone: 'building_b' },
  { id: 'admin_office', name: { vi: 'Phòng Hành chính', en: 'Admin Office' }, category: 'office', waypoint: 'Patrol_F1', zone: 'building_b' },
].map((l) => ({ ...l, building: l.zone === 'building_a' ? buildingA : buildingB, levelId: firstLevel }) as LocationRow)

const areas: Area[] = [
  { id: 'hall_a', name: { vi: 'Sảnh A', en: 'Hall A' }, zone: 'building_a', rmfZone: 'Patrol_C1' },
  { id: 'library_floor', name: { vi: 'Khu đọc Thư viện', en: 'Library reading area' }, zone: 'building_a', rmfZone: 'Patrol_B1' },
  { id: 'corridor_b', name: { vi: 'Hành lang Tòa B', en: 'Building B corridor' }, zone: 'building_b', rmfZone: 'Patrol_D3' },
]

const routes: PatrolRoute[] = [
  { id: 'corridor_a', name: { vi: 'Hành lang Tòa A', en: 'Building A corridor' }, zone: 'building_a', stops: ['library', 'student_center', 'cafeteria'] },
  { id: 'rooms_b', name: { vi: 'Dãy phòng Tòa B', en: 'Building B rooms' }, zone: 'building_b', stops: ['room_118', 'room_312', 'room_204'] },
]

const templates: DeliveryTemplate[] = [
  {
    id: 't-books', service: 'delivery', routeId: null, icon: 'books', kind: 'delivery', stops: [], rounds: 1, pickupId: 'library', dropoffId: 'room_204', packageType: 'documents', available: true,
    name: { vi: 'Thư viện → Phòng 204', en: 'Library to Room 204' }, subtitle: { vi: 'Sách & tài liệu', en: 'Books & Materials' },
  },
  {
    id: 't-lab', service: 'delivery', routeId: null, icon: 'lab', kind: 'delivery', stops: [], rounds: 1, pickupId: 'fablab', dropoffId: 'student_center', packageType: 'lab_equipment', available: true,
    name: { vi: 'FABLAB → Trung tâm SV', en: 'FABLAB to Student Center' }, subtitle: { vi: 'Thiết bị thí nghiệm', en: 'Lab Equipment' },
  },
  {
    id: 't-patrol', service: 'patrol', routeId: 'corridor_a', icon: 'patrol', kind: 'patrol', pickupId: null, dropoffId: null,
    stops: ['library', 'student_center', 'cafeteria'], rounds: 2, packageType: 'general', available: true,
    name: { vi: 'Tuần tra hành lang', en: 'Corridor patrol' }, subtitle: { vi: '3 điểm, 2 vòng', en: '3 stops, 2 rounds' },
  },
]

function robotAt(name: string, fleet: string, waypoint: string, battery: number): RobotRow {
  const p = waypointPoint(firstLevel, waypoint)
  if (!p) throw new Error(`demo seed: waypoint ${waypoint} is not in the nav graph`)
  return { name, fleet, levelId: firstLevel, x: p.x, y: p.y, yaw: 0, battery, deliveryId: null }
}

function locationById(id: string): LocationRow {
  const loc = locations.find((l) => l.id === id)
  if (!loc) throw new Error(`demo seed: unknown location ${id}`)
  return loc
}

/** A task row with the defaults of the first release filled in. */
export function taskRow(patch: Partial<DeliveryRow> & Pick<DeliveryRow, 'id' | 'kind' | 'requesterId' | 'createdAt' | 'status'>): DeliveryRow {
  return {
    service: patch.kind === 'clean' ? 'cleaning' : patch.kind, params: {}, zones: [], priority: 'normal', repeat: 'none', previousId: null,
    requestedRobot: null, startedAt: null, paused: false, pickupId: '', dropoffId: '', stops: [], rounds: 1, roundsDone: 0,
    packageType: 'general', note: '', scheduledAt: null, robotName: null, plan: null, events: [{ type: 'requested', at: patch.createdAt }],
    finishedAt: null, ...patch,
  }
}

function finished(id: number, requesterId: string, pickupId: string, dropoffId: string, robot: string, createdAt: number,
                  packageType: PackageType): DeliveryRow {
  const at = (m: number) => createdAt + m * MIN
  const zonesOf = [...new Set([locationById(pickupId).zone, locationById(dropoffId).zone])]
  return taskRow({
    id, kind: 'delivery', requesterId, pickupId, dropoffId, packageType, createdAt, status: 'completed', robotName: robot,
    params: { pickup: pickupId, dropoff: dropoffId, itemType: packageType, priority: 'normal' }, zones: zonesOf, startedAt: at(0.3), finishedAt: at(8),
    events: [
      { type: 'requested', at: at(0) }, { type: 'assigned', at: at(0.3) }, { type: 'arrived_pickup', at: at(2) }, { type: 'picked_up', at: at(3) },
      { type: 'arriving', at: at(6) }, { type: 'arrived', at: at(7) }, { type: 'completed', at: at(8) },
    ],
  })
}

function finishedRound(id: number, kind: 'patrol' | 'clean', requesterId: string, createdAt: number, robot: string, params: DeliveryRow['params'],
                       zone: string, rounds = 1): DeliveryRow {
  return taskRow({
    id, kind, requesterId, createdAt, status: 'completed', robotName: robot, params, zones: [zone], rounds, roundsDone: rounds,
    stops: kind === 'patrol' ? routes.find((r) => r.id === params.route)!.stops : [],
    pickupId: kind === 'patrol' ? routes.find((r) => r.id === params.route)!.stops[0] : '',
    dropoffId: kind === 'patrol' ? routes.find((r) => r.id === params.route)!.stops.at(-1)! : '',
    startedAt: createdAt + MIN, finishedAt: createdAt + 25 * MIN,
    events: [{ type: 'requested', at: createdAt }, { type: 'assigned', at: createdAt + MIN }, { type: 'completed', at: createdAt + 25 * MIN }],
  })
}

export function seed(now: number): DbData {
  const robots = [
    robotAt('DEL-01', 'eiu_delivery', 'charger_1', 78), robotAt('DEL-02', 'eiu_delivery', 'charger_2', 64), robotAt('DEL-03', 'eiu_delivery', 'charger_3', 91),
    robotAt('CLN-01', 'eiu_cleaning', 'Patrol_A1', 72), robotAt('CLN-02', 'eiu_cleaning', 'Patrol_F1', 17),
    robotAt('PAT-01', 'eiu_patrol', 'Patrol_C3', 83),
  ]
  const day = new Date(now)
  day.setHours(0, 0, 0, 0)
  const today = day.getTime()
  const todayAt = (h: number, m = 0) => Math.min(now - 5 * MIN, today + h * HOUR + m * MIN)

  const deliveries: DeliveryRow[] = [
    finished(1041, 'u-op-a', 'library', 'student_center', 'DEL-03', now - 6 * DAY - 3 * HOUR, 'documents'),
    finished(1042, 'u-op-b', 'cafeteria', 'room_312', 'DEL-01', now - 5 * DAY - HOUR, 'food'),
    finished(1043, 'u-op-a', 'library', 'room_204', 'DEL-02', now - 2 * DAY - 4 * HOUR, 'documents'),
    finished(1044, 'u-op-b', 'cafeteria', 'student_center', 'DEL-01', now - DAY - 2 * HOUR, 'food'),
    finished(1045, 'u-op-a', 'student_center', 'room_118', 'DEL-03', todayAt(9, 30), 'parcel'),
    finished(1046, 'u-op-a', 'room_312', 'fablab', 'DEL-02', now - 2 * HOUR, 'lab_equipment'),
    finished(1047, 'u-op-b', 'library', 'student_center', 'DEL-03', now - 70 * MIN, 'documents'),
    finishedRound(1051, 'clean', 'u-op-a', todayAt(7), 'CLN-01', { area: 'hall_a', cleaningMode: 'full_area' }, 'building_a'),
    finishedRound(1052, 'patrol', 'u-op-c', now - DAY, 'PAT-01', { zone: 'building_a', route: 'corridor_a', rounds: 2 }, 'building_a', 2),
  ]

  const d01 = robots[0]
  const fablab = locationById('fablab')
  const room204 = locationById('room_204')
  const startAt = now - 50_000
  const plan = planDelivery(firstLevel, d01, fablab.waypoint, room204.waypoint, startAt, d01.battery)
  if (!plan) throw new Error('demo seed: no route from DEL-01 through FABLAB to Room 204')
  d01.deliveryId = 1048
  deliveries.push(taskRow({
    id: 1048, kind: 'delivery', requesterId: 'u-op-a', pickupId: 'fablab', dropoffId: 'room_204', packageType: 'lab_equipment',
    params: { pickup: 'fablab', dropoff: 'room_204', itemType: 'lab_equipment', priority: 'high', note: 'Arduino kit, handle with care' },
    zones: ['building_a', 'building_b'], priority: 'high', note: 'Arduino kit, handle with care', createdAt: startAt - 5_000, status: 'to_pickup',
    robotName: 'DEL-01', plan, startedAt: startAt, events: [{ type: 'requested', at: startAt - 5_000 }, { type: 'assigned', at: startAt }],
  }))

  const slot = (offset: number) => Math.ceil((now + offset) / (15 * MIN)) * 15 * MIN
  deliveries.push(
    taskRow({ id: 1049, kind: 'delivery', requesterId: 'u-op-b', pickupId: 'library', dropoffId: 'room_204', packageType: 'documents',
      params: { pickup: 'library', dropoff: 'room_204', itemType: 'documents', priority: 'normal' }, zones: ['building_a', 'building_b'],
      createdAt: now - 30_000, scheduledAt: slot(3 * HOUR), status: 'scheduled' }),
    taskRow({ id: 1050, kind: 'clean', requesterId: 'u-op-a', params: { area: 'library_floor', cleaningMode: 'full_area' }, zones: ['building_a'],
      createdAt: now - 20_000, scheduledAt: slot(5 * HOUR), status: 'scheduled', repeat: 'daily' }),
    taskRow({ id: 1053, kind: 'patrol', requesterId: 'u-op-c', params: { zone: 'building_b', route: 'rooms_b', rounds: 2 }, zones: ['building_b'],
      stops: routes[1].stops, pickupId: routes[1].stops[0], dropoffId: routes[1].stops[2], rounds: 2,
      createdAt: now - 10_000, scheduledAt: slot(7 * HOUR), status: 'scheduled', repeat: 'weekdays' }),
  )

  const name = (id: string) => locationById(id).name
  let nid = 1
  const note = (userId: string, type: NotificationRow['type'], deliveryId: number, createdAt: number,
                params: NotificationRow['params'] = {}): NotificationRow => ({
    id: nid++, userId, type, deliveryId, createdAt, params: { id: deliveryId, ...params },
    readAt: createdAt < now - 2 * HOUR ? createdAt + MIN : null,
  })
  const notifications: NotificationRow[] = [
    ...deliveries.filter((d) => d.status === 'completed' && d.kind === 'delivery').slice(-3).map((d) =>
      note(d.requesterId, 'delivered', d.id, d.finishedAt!, { robot: d.robotName!, dropoff: name(d.dropoffId) })),
    note('u-op-a', 'request_received', 1048, startAt - 5_000),
  ]

  const maintenance: MaintenanceItem[] = [
    { id: 1, robot: 'CLN-01', title: 'Brush replacement', status: 'planned', dueAt: now - 2 * HOUR, windowStart: null, windowEnd: null,
      note: '', createdBy: 'Trần Quốc Bảo', createdAt: now - 3 * DAY, doneAt: null, doneBy: null },
    { id: 2, robot: 'PAT-01', title: 'Camera inspection', status: 'planned', dueAt: now + 2 * DAY, windowStart: today + DAY + 14 * HOUR,
      windowEnd: today + DAY + 15 * HOUR, note: '', createdBy: 'Trần Quốc Bảo', createdAt: now - DAY, doneAt: null, doneBy: null },
  ]

  return {
    version: DB_VERSION,
    users: users.map((u) => ({ ...u, notificationPrefs: { ...u.notificationPrefs } })),
    session: null,
    levelLabels: { [firstLevel]: { vi: 'Tầng 1', en: '1F' } },
    locations,
    saved: {},
    templates,
    robots,
    deliveries,
    notifications,
    nextDeliveryId: 1054,
    nextNotificationId: nid,
    zones,
    areas,
    routes,
    maintenance,
    nextMaintenanceId: 3,
    toggles: {},
  }
}
