// Tables of the demo database. The backend's PostgreSQL schema starts from the same shapes.
import type {
  Area,
  MaintenanceItem,
  OperatorPermission,
  PatrolRoute,
  Repeat,
  DeliveryStatus,
  DeliveryTemplate,
  I18nText,
  Locale,
  LocationCategory,
  NotificationPrefs,
  NotificationType,
  PackageType,
  Role,
  TaskKind,
  SavedLocation,
} from '../api/types'
import type { Milestone, Plan } from './plan'

export interface UserRow {
  id: string
  email: string
  /** Demo only: the backend stores an Argon2id hash. */
  password: string
  fullName: string
  role: Role
  department: string
  allowedServices: string[]
  /** Empty = every zone. */
  allowedZones: string[]
  permissions: OperatorPermission[]
  locale: Locale
  notificationPrefs: NotificationPrefs
  active: boolean
  createdAt?: number
  lastLoginAt?: number | null
  lastActiveAt?: number | null
}

export interface LocationRow {
  id: string
  name: I18nText
  building: I18nText
  category: LocationCategory
  zone: string
  levelId: string
  waypoint: string
}

export interface ZoneRow {
  id: string
  name: I18nText
  levelId: string
  enabled: boolean
}

export interface RobotRow {
  name: string
  fleet: string
  levelId: string
  x: number
  y: number
  yaw: number
  battery: number
  deliveryId: number | null
}

export type DeliveryEventType = 'requested' | 'assigned' | Milestone | 'cancelled' | 'failed' | 'delayed' | 'started' | 'reassigned' | 'paused' | 'resumed'

export interface DeliveryRow {
  id: number
  kind: TaskKind
  service: string
  params: Record<string, string | number | string[]>
  zones: string[]
  priority: string
  repeat: Repeat
  previousId: number | null
  requestedRobot: string | null
  startedAt: number | null
  paused: boolean
  requesterId: string
  pickupId: string
  dropoffId: string
  /** Patrol stops in order; empty for a delivery. */
  stops: string[]
  rounds: number
  roundsDone: number
  packageType: PackageType
  note: string
  createdAt: number
  scheduledAt: number | null
  status: DeliveryStatus
  robotName: string | null
  plan: Plan | null
  events: { type: DeliveryEventType; at: number; detail?: string }[]
  finishedAt: number | null
  /** Time already travelled when the task was paused; the plan restarts from there on resume. */
  pausedAt?: number | null
}

export interface NotificationRow {
  id: number
  userId: string
  type: NotificationType
  deliveryId: number | null
  params: Record<string, string | number | I18nText>
  createdAt: number
  readAt: number | null
}

export interface Session {
  userId: string
  csrf: string
}

export interface AuditRow {
  id: number
  at: number
  actorId: string | null
  action: string
  target: string
  detail: string
}

export interface DbData {
  version: number
  users: UserRow[]
  session: Session | null
  levelLabels: Record<string, I18nText>
  locations: LocationRow[]
  saved: Record<string, SavedLocation[]>
  templates: DeliveryTemplate[]
  robots: RobotRow[]
  deliveries: DeliveryRow[]
  notifications: NotificationRow[]
  nextDeliveryId: number
  nextNotificationId: number
  /** Serials of discovered demo robots that were registered. */
  registeredSerials?: string[]
  /** Audit log of the demo, newest first. */
  audit?: AuditRow[]
  zones: ZoneRow[]
  areas: Area[]
  routes: PatrolRoute[]
  maintenance: MaintenanceItem[]
  nextMaintenanceId: number
  /** Admin switches: `service:<id>` or `zone:<id>`. */
  toggles: Record<string, boolean>
}
