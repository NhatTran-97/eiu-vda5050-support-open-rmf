// Services and authorization of the demo backend; the same rules as backend/eiu_web_backend/access.py and services.py.
import type { FormField, Me, Permission, ServiceDef } from '../api/types'
import { db } from './db'
import type { DeliveryRow, UserRow } from './schema'

export { ADMIN_PERMISSIONS, DEFAULT_OPERATOR, GRANTABLE, OPERATOR_PERMISSIONS } from './permissions'
import { ADMIN_PERMISSIONS, GRANTABLE } from './permissions'

type Service = Omit<ServiceDef, 'allowed' | 'available' | 'fleets'>

const note: FormField = { key: 'note', type: 'text', required: false, max: 200 }

/** config/services.yaml of the demo. */
export const SERVICES: Service[] = [
  {
    id: 'delivery', icon: 'package', enabled: true, category: 'delivery', requiredCapabilities: ['transport'],
    name: { vi: 'Giao vận', en: 'Delivery' },
    description: { vi: 'Vận chuyển tài liệu, bưu kiện và thiết bị', en: 'Transport documents, parcels and equipment' },
    taskFormSchema: [
      { key: 'pickup', type: 'location', required: true },
      { key: 'dropoff', type: 'location', required: true, differsFrom: 'pickup' },
      { key: 'itemType', type: 'select', required: false, options: ['documents', 'parcel', 'lab_equipment', 'food', 'general'], default: 'general' },
      { key: 'priority', type: 'select', required: false, options: ['normal', 'high'], default: 'normal' },
      { key: 'schedule', type: 'schedule', required: false, repeat: false },
      note,
    ],
  },
  {
    id: 'cleaning', icon: 'sparkles', enabled: true, category: 'clean', requiredCapabilities: ['cleaning'],
    name: { vi: 'Vệ sinh', en: 'Cleaning' },
    description: { vi: 'Dịch vụ vệ sinh tự động', en: 'Autonomous cleaning service' },
    taskFormSchema: [
      { key: 'area', type: 'area', required: true },
      { key: 'cleaningMode', type: 'select', required: false, options: ['full_area', 'spot', 'custom_zone'], default: 'full_area' },
      { key: 'duration', type: 'number', required: false, min: 5, max: 240, unit: 'min' },
      { key: 'schedule', type: 'schedule', required: false, repeat: true },
      note,
    ],
  },
  {
    id: 'patrol', icon: 'shield', enabled: true, category: 'patrol', requiredCapabilities: ['patrol'],
    name: { vi: 'Tuần tra', en: 'Patrol' },
    description: { vi: 'Nhiệm vụ tuần tra tự động', en: 'Autonomous patrol missions' },
    taskFormSchema: [
      { key: 'zone', type: 'zone', required: true },
      { key: 'route', type: 'route', required: true, filter: 'zone' },
      { key: 'rounds', type: 'number', required: false, min: 1, max: 10, default: 1 },
      { key: 'schedule', type: 'schedule', required: false, repeat: true },
      note,
    ],
  },
]

/** Fleet name: primary service and capabilities of its robots. */
export const FLEETS: Record<string, { service: string; capabilities: string[] }> = {
  eiu_delivery: { service: 'delivery', capabilities: ['transport'] },
  eiu_cleaning: { service: 'cleaning', capabilities: ['cleaning'] },
  eiu_patrol: { service: 'patrol', capabilities: ['patrol'] },
}

export class Forbidden extends Error {
  constructor(readonly status: number, readonly code: string, readonly detail = '') {
    super(code)
  }
}

const MESSAGES: Record<string, { en: string; vi: string }> = {
  PERMISSION_DENIED: { en: 'You do not have permission for this action.', vi: 'Bạn không có quyền thực hiện thao tác này.' },
  ADMIN_ONLY: { en: 'Only an administrator can do this.', vi: 'Chỉ quản trị viên được thực hiện thao tác này.' },
  SERVICE_NOT_ALLOWED: { en: 'You do not have permission to create {service} tasks.', vi: 'Bạn không có quyền tạo nhiệm vụ {service}.' },
  SERVICE_DISABLED: { en: 'The {service} service is disabled.', vi: 'Dịch vụ {service} đang tắt.' },
  ZONE_NOT_ALLOWED: { en: 'You do not have access to {zone}.', vi: 'Bạn không có quyền ở khu vực {zone}.' },
  ZONE_DISABLED: { en: '{zone} is closed to robot tasks.', vi: 'Khu vực {zone} đang đóng với nhiệm vụ robot.' },
  NO_CAPABLE_FLEET: { en: 'No robot fleet can perform {service} tasks.', vi: 'Chưa có đội robot nào thực hiện được nhiệm vụ {service}.' },
  ROBOT_NOT_CAPABLE: { en: '{robot} cannot perform {service} tasks.', vi: '{robot} không thực hiện được nhiệm vụ {service}.' },
}

export function deny(user: UserRow | null, status: number, code: string, params: Record<string, string> = {}): Forbidden {
  const text = MESSAGES[code][user?.locale ?? 'en'].replace(/\{(\w+)\}/g, (_, k: string) => params[k] ?? '')
  return new Forbidden(status, code, text)
}

export function serviceOf(id: string): Service | undefined {
  return SERVICES.find((s) => s.id === id)
}

export function serviceEnabled(s: Service): boolean {
  return db().toggles[`service:${s.id}`] ?? s.enabled
}

export function zoneEnabled(id: string): boolean {
  return db().toggles[`zone:${id}`] ?? db().zones.find((z) => z.id === id)?.enabled ?? true
}

export function permissionsOf(user: UserRow): Permission[] {
  if (user.role === 'admin') return [...GRANTABLE, ...ADMIN_PERMISSIONS]
  return GRANTABLE.filter((p) => user.permissions.includes(p))
}

export function can(user: UserRow, p: Permission): boolean {
  return user.active && permissionsOf(user).includes(p)
}

export function require(user: UserRow, p: Permission): void {
  if (!can(user, p)) throw deny(user, 403, ADMIN_PERMISSIONS.includes(p) ? 'ADMIN_ONLY' : 'PERMISSION_DENIED')
}

export function servicesOf(user: UserRow): string[] {
  const enabled = SERVICES.filter(serviceEnabled).map((s) => s.id)
  return user.role === 'admin' ? enabled : enabled.filter((s) => user.allowedServices.includes(s))
}

/** null = every zone. */
export function zonesOf(user: UserRow): Set<string> | null {
  return user.role === 'admin' || user.allowedZones.length === 0 ? null : new Set(user.allowedZones)
}

export function seesAll(user: UserRow): boolean {
  return user.role === 'admin' || can(user, 'fleet.view_all')
}

export function fleetCapabilities(fleet: string): string[] {
  return FLEETS[fleet]?.capabilities ?? []
}

export function fleetServices(fleet: string): string[] {
  const caps = new Set(fleetCapabilities(fleet))
  return SERVICES.filter((s) => caps.size && s.requiredCapabilities.every((c) => caps.has(c))).map((s) => s.id)
}

export function fleetVisible(user: UserRow, fleet: string): boolean {
  if (seesAll(user)) return true
  const mine = new Set(servicesOf(user))
  return fleetServices(fleet).some((s) => mine.has(s))
}

export function capableFleets(s: Service): string[] {
  const known = new Set([...Object.keys(FLEETS), ...db().robots.map((r) => r.fleet)])
  return [...known].filter((f) => s.requiredCapabilities.every((c) => fleetCapabilities(f).includes(c))).sort()
}

export function taskVisible(user: UserRow, row: DeliveryRow): boolean {
  if (seesAll(user) || row.requesterId === user.id) return true
  if (!servicesOf(user).includes(row.service)) return false
  const zones = zonesOf(user)
  return zones === null || row.zones.every((z) => zones.has(z))
}

function name(user: UserRow, s: Service): string {
  return s.name[user.locale].toLowerCase()
}

export function checkService(user: UserRow, s: Service | undefined): Service {
  if (!user.active) throw deny(user, 403, 'PERMISSION_DENIED')
  if (!s) throw new Forbidden(422, 'task.unknown_service')
  if (!serviceEnabled(s)) throw deny(user, 409, 'SERVICE_DISABLED', { service: name(user, s) })
  if (!servicesOf(user).includes(s.id)) throw deny(user, 403, 'SERVICE_NOT_ALLOWED', { service: name(user, s) })
  return s
}

export function checkScope(user: UserRow, s: Service, zones: Set<string>): void {
  const allowed = zonesOf(user)
  for (const z of [...zones].sort()) {
    const label = db().zones.find((x) => x.id === z)?.name[user.locale] ?? z
    if (allowed && !allowed.has(z)) throw deny(user, 403, 'ZONE_NOT_ALLOWED', { zone: label })
    if (!zoneEnabled(z)) throw deny(user, 409, 'ZONE_DISABLED', { zone: label })
  }
  require(user, 'task.create')
  if (capableFleets(s).length === 0) throw deny(user, 409, 'NO_CAPABLE_FLEET', { service: name(user, s) })
}

export function checkRobot(user: UserRow, s: Service, robot: string, fleet: string, permission: Permission = 'fleet.assign'): void {
  require(user, permission)
  if (!fleetVisible(user, fleet) || !s.requiredCapabilities.every((c) => fleetCapabilities(fleet).includes(c))) {
    throw deny(user, 409, 'ROBOT_NOT_CAPABLE', { robot, service: name(user, s) })
  }
}

/** Parameters checked against the service's form, defaults filled in, and the zones they touch. */
export function validate(s: Service, params: Record<string, unknown>): { values: DeliveryRow['params']; zones: Set<string> } {
  const data = db()
  const values: DeliveryRow['params'] = {}
  const zones = new Set<string>()
  for (const f of s.taskFormSchema) {
    if (f.type === 'schedule') continue
    const v = params[f.key]
    if (v === undefined || v === null || v === '' || (Array.isArray(v) && v.length === 0)) {
      if (f.default !== undefined) values[f.key] = f.default
      else if (f.required) throw new Forbidden(422, 'task.field_required', f.key)
      continue
    }
    if (f.type === 'location') {
      const loc = data.locations.find((l) => l.id === v)
      if (!loc) throw new Forbidden(422, 'task.unknown_location', f.key)
      values[f.key] = loc.id
      zones.add(loc.zone)
    } else if (f.type === 'area') {
      const area = data.areas.find((a) => a.id === v)
      if (!area) throw new Forbidden(422, 'task.unknown_area', f.key)
      values[f.key] = area.id
      zones.add(area.zone)
    } else if (f.type === 'route') {
      const route = data.routes.find((r) => r.id === v)
      if (!route) throw new Forbidden(422, 'task.unknown_route', f.key)
      values[f.key] = route.id
      zones.add(route.zone)
    } else if (f.type === 'zone') {
      if (!data.zones.some((z) => z.id === v)) throw new Forbidden(422, 'task.unknown_zone', f.key)
      values[f.key] = String(v)
      zones.add(String(v))
    } else if (f.type === 'select') {
      if (!f.options?.includes(String(v))) throw new Forbidden(422, 'task.invalid_option', f.key)
      values[f.key] = String(v)
    } else if (f.type === 'number') {
      if (typeof v !== 'number' || v < (f.min ?? 0) || v > (f.max ?? 1e9)) throw new Forbidden(422, 'task.invalid_number', f.key)
      values[f.key] = v
    } else if (f.type === 'text') {
      const text = String(v).trim()
      if (text.length > (f.max ?? 200)) throw new Forbidden(422, 'task.text_too_long', f.key)
      values[f.key] = text
    } else if (f.type === 'locations') {
      const list = (v as string[]).map(String)
      if (!list.every((id) => data.locations.some((l) => l.id === id))) throw new Forbidden(422, 'task.unknown_location', f.key)
      values[f.key] = list
      list.forEach((id) => zones.add(data.locations.find((l) => l.id === id)!.zone))
    }
  }
  for (const f of s.taskFormSchema) {
    if (f.differsFrom && values[f.key] !== undefined && values[f.key] === values[f.differsFrom]) throw new Forbidden(422, 'task.same_location', f.key)
    if (f.filter && values[f.key] && values[f.filter]) {
      const item = f.type === 'route' ? data.routes.find((r) => r.id === values[f.key]) : data.areas.find((a) => a.id === values[f.key])
      if (item && item.zone !== values[f.filter]) throw new Forbidden(422, 'task.not_in_zone', f.key)
    }
  }
  return { values, zones }
}

export function toMe(user: UserRow): Me {
  const zones = zonesOf(user)
  return {
    id: user.id, email: user.email, fullName: user.fullName, role: user.role, department: user.department,
    permissions: permissionsOf(user), allowedServices: servicesOf(user), allowedZones: zones ? [...zones].sort() : [], allZones: zones === null,
    locale: user.locale, notificationPrefs: { ...user.notificationPrefs },
  }
}
