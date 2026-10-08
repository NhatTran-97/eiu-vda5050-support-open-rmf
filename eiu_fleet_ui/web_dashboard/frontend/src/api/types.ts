// Data contract between the frontend and the backend (REST under /api/v1 and the realtime stream).
// Times are epoch milliseconds (UTC); positions are map metres on a level.

export type Locale = 'vi' | 'en'

/** Text the server stores in every supported language. */
export type I18nText = Record<Locale, string>

/** Operator permissions an admin grants; the admin role has all of them plus the admin-only ones. */
export type OperatorPermission =
  | 'task.create'
  | 'task.cancel'
  | 'task.schedule'
  | 'task.pause'
  | 'task.reassign'
  | 'fleet.view'
  | 'fleet.view_all'
  | 'fleet.assign'
  | 'fleet.control'
  | 'alerts.ack'
  | 'analytics.view'
  | 'maintenance.view'

export type AdminPermission =
  | 'users.manage'
  | 'robots.manage'
  | 'locations.manage'
  | 'maintenance.manage'
  | 'integrations.manage'
  | 'settings.manage'
  | 'system.diagnostics'

export type Permission = OperatorPermission | AdminPermission

export type Role = 'operator' | 'admin'

export interface NotificationPrefs {
  deliveryUpdates: boolean
  delays: boolean
}

export interface Me {
  id: string
  email: string
  fullName: string
  role: Role
  department: string
  /** Effective permissions: all of them for an admin. */
  permissions: Permission[]
  /** Enabled services the user may operate: all enabled ones for an admin. */
  allowedServices: string[]
  /** Zones the user may send robots to; meaningful only when `allZones` is false. */
  allowedZones: string[]
  allZones: boolean
  locale: Locale
  notificationPrefs: NotificationPrefs
}

export interface SiteConfig {
  siteName: I18nText
  timeZone: string
  defaultLocale: Locale
  locales: Locale[]
  maxActiveDeliveries: number
  minPasswordLength: number
  maxPatrolStops: number
  maxPatrolRounds: number
  support: { email: string; phone: string; hours: I18nText }
  demo: boolean
  /** Sample sign-ins, present only on the demo backend. */
  demoAccounts?: { role: Role; email: string; password: string; name?: string }[]
}

// Map

export interface Level {
  id: string
  label: I18nText
  imageUrl: string
  origin: [number, number]
  resolution: number
  widthPx: number
  heightPx: number
}

export interface GraphVertex {
  name: string
  x: number
  y: number
  charger: boolean
}

export interface LevelGraph {
  levelId: string
  vertices: GraphVertex[]
  lanes: [number, number][]
}

// Locations

export type LocationCategory = 'lab' | 'library' | 'center' | 'room' | 'cafeteria' | 'office'

export interface Location {
  id: string
  name: I18nText
  building: I18nText
  category: LocationCategory
  zone: string
  levelId: string
  waypoint: string
  x: number
  y: number
}

export interface SavedLocation {
  locationId: string
  starred: boolean
}

export interface RecentDestination {
  locationId: string
  lastUsedAt: number
}

export type PackageType = 'general' | 'documents' | 'parcel' | 'food' | 'lab_equipment' | 'fragile'

/** RMF task category of a service. */
export type TaskKind = 'delivery' | 'patrol' | 'clean'

export interface DeliveryTemplate {
  id: string
  /** Service the template fills the form of. */
  service: string
  routeId: string | null
  name: I18nText
  subtitle: I18nText
  icon: 'books' | 'lab' | 'food' | 'general' | 'qr' | 'patrol'
  kind: TaskKind
  pickupId: string | null
  dropoffId: string | null
  stops: string[]
  rounds: number
  packageType: PackageType
  available: boolean
}

// Deliveries

export type DeliveryStatus =
  | 'scheduled'
  | 'queued'
  | 'to_pickup'
  | 'at_pickup'
  | 'in_transit'
  | 'arrived'
  | 'underway'
  | 'completed'
  | 'cancelled'
  | 'failed'

export type DeliveryGroup = 'active' | 'upcoming' | 'completed'

/** Delivery: requested, picked_up, en_route, arriving, delivered. Patrol: requested, assigned, patrolling. Clean: requested, assigned, cleaning. */
export type TimelineKey = 'requested' | 'picked_up' | 'en_route' | 'arriving' | 'delivered' | 'assigned' | 'patrolling' | 'cleaning'

export interface TimelineStep {
  key: TimelineKey
  state: 'done' | 'current' | 'pending' | 'skipped'
  at: number | null
}

export interface LocationRef {
  id: string
  name: I18nText
  category: LocationCategory
  levelId: string
}

/** Status of the task model shown everywhere; `status` is the finer stage of the task. */
export type TaskState = 'SCHEDULED' | 'QUEUED' | 'ASSIGNED' | 'EXECUTING' | 'PAUSED' | 'COMPLETED' | 'CANCELLED' | 'FAILED'

export type Repeat = 'none' | 'daily' | 'weekdays' | 'weekly'

export interface NamedRef {
  id: string
  name: I18nText
  zone: string
}

/** A robot task of a service: delivery (pickup -> drop-off), patrol (route x rounds) or cleaning (an area). */
export interface Delivery {
  id: number
  kind: TaskKind
  service: string
  state: TaskState
  priority: string
  /** Form values of the service, by field key. */
  parameters: Record<string, string | number | string[]>
  zones: string[]
  area: NamedRef | null
  route: NamedRef | null
  repeat: Repeat
  /** Earlier task of a repeat series, or the task this one replaced. */
  previousId: number | null
  requestedRobot: string | null
  startedAt: number | null
  status: DeliveryStatus
  group: DeliveryGroup
  pickup: LocationRef
  dropoff: LocationRef
  /** Patrol stops in order; empty for a delivery. */
  stops: LocationRef[]
  rounds: number
  roundsDone: number
  packageType: PackageType
  note: string
  requester: { id: string; fullName: string }
  robot: { name: string; fleet: string } | null
  createdAt: number
  scheduledAt: number | null
  finishedAt: number | null
  /** Expected arrival at the drop-off. */
  etaAt: number | null
  /** Error code or RMF error text of a failed task. */
  error: string | null
  timeline: TimelineStep[]
  actions: { cancel: boolean; pause: boolean; resume: boolean; reassign: boolean; track: boolean; reorder: boolean }
}

export interface DeliveryList {
  items: Delivery[]
  counts: Record<'all' | DeliveryGroup, number>
  nextCursor: string | null
}

export interface NewTask {
  serviceType: string
  parameters: Record<string, string | number | string[]>
  scheduledAt: number | null
  repeat?: Repeat
  /** Manual assignment (permission fleet.assign). */
  robot?: string
}

// Notifications

export type NotificationType =
  | 'request_received'
  | 'robot_at_pickup'
  | 'package_loaded'
  | 'near_destination'
  | 'robot_arrived'
  | 'delivered'
  | 'cancelled'
  | 'delayed'
  | 'action_required'
  | 'patrol_started'
  | 'patrol_completed'
  | 'task_failed'

export interface AppNotification {
  id: number
  type: NotificationType
  severity: AlertSeverity
  deliveryId: number | null
  /** Values for the message template, e.g. robot and location names. */
  params: Record<string, string | number | I18nText>
  createdAt: number
  readAt: number | null
}

export interface NotificationList {
  items: AppNotification[]
  unread: number
  nextCursor: string | null
}

// Realtime

export type RobotActivity = 'idle' | 'moving' | 'waiting' | 'offline'

export interface RobotLive {
  name: string
  fleet: string
  levelId: string
  x: number
  y: number
  yaw: number
  battery: number
  activity: RobotActivity
  deliveryId: number | null
  /** Metres left to the drop-off of the current delivery. */
  remainingM: number | null
  /** Remaining route as [x, y] points. */
  path: [number, number][]
}

export type RealtimeTopic = 'deliveries' | 'notifications' | 'fleet' | 'alerts' | 'access'

/** Link between the backend and Open-RMF. */
export type RmfLink = 'online' | 'offline' | 'unavailable' | 'disabled'

export type ServerMessage =
  | { type: 'patch'; topic: 'robots'; seq: number; periodMs: number; upsert: RobotLive[]; remove: string[] }
  | { type: 'event'; topics: RealtimeTopic[] }
  | { type: 'system'; rmf: RmfLink }

export interface ApiErrorBody {
  error: { code: string; message?: string }
}

// Operations (role permissions fleet:* and map:edit)

export type RobotStatus = 'IDLE' | 'NAVIGATING' | 'EXECUTING' | 'CHARGING' | 'PAUSED' | 'MAINTENANCE' | 'OFFLINE' | 'ERROR'

export type RobotHealth = 'healthy' | 'warning' | 'critical'

/** Running task of a robot, as the robot views show it. */
export interface LiveTask {
  id: number
  service: string
  kind: TaskKind
  state: TaskState
  status: DeliveryStatus
  pickupId: string
  dropoffId: string
  stops: string[]
  areaId: string | null
  routeId: string | null
  rounds: number
  roundsDone: number
  etaAt: number | null
  zones: string[]
}

export interface FleetRobot {
  name: string
  status: RobotStatus
  health: RobotHealth
  /** Primary service of the robot's fleet (its type); null when the fleet is not configured. */
  serviceType: string | null
  /** Services the robot's capabilities cover. */
  services: string[]
  capabilities: string[]
  connection: 'online' | 'offline'
  onlineSince: number | null
  lastUpdateAt: number | null
  locationId: string | null
  task: LiveTask | null
  maintenance: boolean
  /** Service telemetry the robot reports (water tank, brush, camera...); empty when it reports none. */
  telemetry: Record<string, string | number | boolean>
  fleet: string
  levelId: string
  x: number
  y: number
  yaw: number
  battery: number
  activity: RobotActivity
  /** RMF robot mode: idle, charging, moving, paused, waiting, emergency, going_home, docking, adapter_error, cleaning; offline when stale. */
  mode: string
  taskId: string | null
  deliveryId: number | null
  /** Node of the fleet adapter that offers this robot's controls. */
  adapter: string | null
  controls: boolean
  /** Operator speed cap in m/s; 0 = none, null = unknown. */
  speedLimit: number | null
  /** Result of the last pause or resume sent through the gateway; null = unknown. */
  paused: boolean | null
}

export type RobotAction = 'pause' | 'resume' | 'speed-limit' | 'init-position'

export interface RegistryRobot {
  name: string
  manufacturer: string
  serial: string
  charger: string
  retired?: boolean
  source?: string
}

export interface FleetRegistry {
  fleet: string
  /** Node of the fleet adapter that owns the fleet. */
  adapter_node?: string
  interface?: string
  series?: string
  limits?: Record<string, number>
  robots: RegistryRobot[]
  chargers: { name: string; used_by?: string; used_by_removed?: boolean }[]
}

export interface PendingRobot {
  manufacturer: string
  serial: string
  series?: string
  pose?: { x: number; y: number; map: string; initialized: boolean }
  removed_as?: { fleet: string; name: string; charger: string }
  reporters: string[]
  suggestion: { fleet: string; name: string; charger: string }
}

export interface RegistrationView {
  fleets: FleetRegistry[]
  pending: PendingRobot[]
}

export interface RegistrationRequest {
  action: 'check' | 'add' | 'remove'
  fleet: string
  name: string
  manufacturer?: string
  serial?: string
  charger?: string
  responsiveWait?: boolean
  confirmUnverified?: boolean
}

export interface Finding {
  code: string
  message: string
}

export interface RegistrationResult {
  ok: boolean
  dryRun: boolean
  persisted: boolean
  needsConfirmation: boolean
  errors: Finding[]
  warnings: Finding[]
}

export interface LanesView {
  /** Closed lane indices of each fleet, numbered across the whole nav graph as RMF does. */
  fleets: Record<string, number[]>
  /** Index of each level's first lane in that numbering. */
  offsets: Record<string, number>
}

export interface GraphVertexEdit {
  name: string
  x: number
  y: number
  charger: boolean
  attrs: Record<string, unknown>
}

export interface GraphLaneEdit {
  from: number
  to: number
  attrs: Record<string, unknown>
}

export interface NavGraphView {
  available: boolean
  path?: string
  sha256?: string
  levels?: string[]
  levelId?: string
  vertices?: GraphVertexEdit[]
  lanes?: GraphLaneEdit[]
}

export interface AdapterSystem {
  node: string
  fleet: string
  status: 'ok' | 'warning' | 'critical' | 'silent' | 'waiting' | 'found' | 'absent'
  reported_ago_s: number | null
  interval_s: number
  uptime_s: number
  robots: { registered: number; online: number; state_age_max_s: number; oldest_state_robot: string }
  mqtt: { connected: boolean; connections_lost: number }
  totals: { rx: number; unregistered: number; dropped: number; published_failed: number }
  period_ms: number
  series: { age_s: number[]; msg_per_s: (number | null)[]; loop_p99_ms: (number | null)[] }
}

export interface SystemView {
  rmf: RmfLink
  gateway: { ros_domain_id?: string; started_ms?: number; task_events?: boolean; nav_graph_path?: string }
  summary: { total: number; found: number; level: 'wait' | 'ok' | 'warn' | 'err' }
  attention: { key: string; severity: 'warning' | 'critical'; title: string; detail: string }[]
  adapters: AdapterSystem[]
}

export type HealthStatus = 'ok' | 'warning' | 'critical' | 'unknown'

export interface HealthItem {
  key: 'database' | 'gateway' | 'rmf' | 'task_events' | 'fleet' | 'adapter' | 'mqtt'
  status: HealthStatus
  params: Record<string, string | number>
  /** Figures behind the status; keys ending in _s are durations in seconds, null = not known. */
  details: Record<string, string | number | boolean | null>
}

export interface AlertCounts {
  open: number
  unacked: number
  critical: number
}

export interface Overview {
  robots: { total: number; online: number; offline: number; moving: number; idle: number; charging: number; waiting: number; paused: number }
  tasks: { active: number; queued: number; scheduled: number; completedToday: number; failedToday: number; cancelledToday: number; byKind: Record<string, number> }
  alerts: AlertCounts
  health: HealthItem[]
}

export type TaskGroup = 'active' | 'scheduled' | 'finished' | 'all'

export interface FleetTask extends Delivery {
  rmfTaskId: string | null
}

export interface TaskRef {
  id: number
  kind: TaskKind
  service: string
  pickupId: string
  dropoffId: string
  stops: string[]
  robot: string | null
  /** When the task became due (created or scheduled time). */
  since: number
  etaAt: number | null
  lateMin?: number
}

export interface FleetTaskList {
  items: FleetTask[]
  counts: Record<TaskGroup, number>
  summary: { active: number; queued: number; scheduled: number; completedToday: number; failedToday: number; avgDurationMin: number | null; late: number }
  /** Queued tasks without a robot, oldest first. */
  unassigned: TaskRef[]
  /** Running tasks past their ETA, latest first. */
  late: TaskRef[]
}

export type AlertSeverity = 'critical' | 'warning' | 'info'

export interface OpsAlert {
  id: number
  key: string
  /** condition: closes when the condition ends; event: closed by an operator. */
  kind: 'condition' | 'event'
  severity: AlertSeverity
  code: string
  params: Record<string, string | number>
  robot: string | null
  deliveryId: number | null
  openedAt: number
  updatedAt: number
  ackedAt: number | null
  ackedBy: string | null
  resolvedAt: number | null
  /** Who closed it; null when it closed by itself. */
  resolvedBy: string | null
  /** The condition still holds (or the event is open). */
  active: boolean
}

export interface AlertList {
  items: OpsAlert[]
  counts: AlertCounts
}

// Administration (permission admin:access; accounts need user:manage)

export interface AdminUser {
  id: string
  email: string
  fullName: string
  role: Role
  department: string
  active: boolean
  locale: Locale
  allowedServices: string[]
  /** Empty = every zone. */
  allowedZones: string[]
  permissions: Permission[]
  createdAt: number
  lastLoginAt: number | null
  lastActiveAt: number | null
}

export interface AccessCatalog {
  services: { id: string; name: I18nText; icon: ServiceIcon; enabled: boolean }[]
  zones: { id: string; name: I18nText; enabled: boolean }[]
  permissionGroups: { id: 'task' | 'fleet' | 'insight'; permissions: OperatorPermission[] }[]
  adminPermissions: AdminPermission[]
  defaults: OperatorPermission[]
}

export type UserAccessPatch = Partial<Pick<AdminUser, 'fullName' | 'role' | 'active' | 'department' | 'allowedServices' | 'allowedZones' | 'permissions'>>

export interface RoleInfo {
  role: Role
  permissions: Permission[]
}

export interface AuditEntry {
  id: number
  at: number
  /** Name of the user who acted; null for the command line or the system. */
  actor: string | null
  action: string
  target: string
  detail: string
}

export interface AuditList {
  items: AuditEntry[]
  /** Every action name in the log, for the filter. */
  actions: string[]
}

export interface AdminOverview extends Overview {
  fleets: { configured: number; healthy: number }
  maps: { levels: number; editable: boolean; navGraphPath: string | null }
  integrations: { total: number; healthy: number }
  users: { active: number; admins: number; operators: number }
  recentChanges: AuditEntry[]
}

export interface Infrastructure {
  /** Nav graph file the lists come from. */
  source: string
  chargers: { name: string; level: string; x: number; y: number; usedBy: string | null }[]
  doors: { name: string; level: string; type: string }[]
  lifts: { name: string; levels: string[] }[]
}

export interface UtilizationDay {
  /** YYYY-MM-DD in the site's time zone. */
  day: string
  /** Hours in each state. */
  active: number
  idle: number
  charging: number
  offline: number
}

export interface RobotEvent {
  at: number
  source: 'task' | 'alert' | 'audit'
  level: 'critical' | 'warning' | 'info'
  code: string
  deliveryId?: number
  params?: Record<string, string | number>
  actor?: string | null
  text?: string
  closedAt?: number | null
}

export interface RobotDetailView {
  robot: FleetRobot
  /** Keys ending in _s are seconds; _m metres; _mps m/s. */
  technical: Record<string, string | number | boolean | null>
  tasks: { id: number; kind: TaskKind; status: DeliveryStatus; pickupId: string; dropoffId: string; createdAt: number; finishedAt: number | null }[]
  events: RobotEvent[]
  /** [epoch ms, %] samples. */
  battery: [number, number][]
  hours: number
  utilization: UtilizationDay[]
  sampleS: number
}

export interface AnalyticsReport {
  days: number
  kpis: { created: number; completed: number; failed: number; cancelled: number; successRate: number | null
    avgDurationMin: number | null; avgWaitMin: number | null; utilizationPct: number | null; alerts: number }
  tasksPerDay: { day: string; created: number; completed: number; failed: number; cancelled: number }[]
  topPlaces: { id: string; name: I18nText; count: number }[]
  utilization: UtilizationDay[]
  battery: { from: number; to: number; robots: number }[]
  alertsPerDay: { day: string; critical: number; warning: number; info: number }[]
  alertCodes: { code: string; count: number }[]
  sampleS: number
  byService: ServiceFigures[]
}

/** Figures of one service; null where no robot reports the figure. */
export interface ServiceFigures {
  id: string
  category: TaskKind
  created: number
  completed: number
  avgDurationMin: number | null
  distanceKm?: number | null
  areaM2?: number | null
  coveragePct?: number | null
  areas?: number
  rounds?: number
  zonesCovered?: number
  avgRoundMin?: number | null
}

// Services and their task forms

export type ServiceIcon = 'package' | 'sparkles' | 'shield' | 'bot' | 'truck' | 'camera' | 'wrench'

export type FormFieldType = 'location' | 'locations' | 'area' | 'route' | 'zone' | 'select' | 'number' | 'text' | 'schedule'

export interface FormField {
  key: string
  type: FormFieldType
  required: boolean
  options?: string[]
  default?: string | number
  min?: number
  max?: number
  unit?: string
  maxItems?: number
  /** Schedule field: the service allows a repeat. */
  repeat?: boolean
  /** Key of a field whose value must differ (pickup and drop-off). */
  differsFrom?: string
  /** Key of the zone field that narrows this route or area field. */
  filter?: string
  label?: I18nText
}

export interface ServiceDef {
  id: string
  name: I18nText
  description: I18nText
  icon: ServiceIcon
  enabled: boolean
  category: TaskKind
  requiredCapabilities: string[]
  taskFormSchema: FormField[]
  /** The viewer may operate the service. */
  allowed: boolean
  /** A fleet with the required capabilities is configured or reporting. */
  available: boolean
  fleets: string[]
}

export interface Zone {
  id: string
  name: I18nText
  levelId: string
  enabled: boolean
  allowed: boolean
}

export interface Area {
  id: string
  name: I18nText
  zone: string
  rmfZone: string
}

export interface PatrolRoute {
  id: string
  name: I18nText
  zone: string
  stops: string[]
}

export interface Catalog {
  zones: Zone[]
  areas: Area[]
  routes: PatrolRoute[]
}

// Operations overview

export type RobotGroup = 'active' | 'idle' | 'charging' | 'paused' | 'maintenance' | 'offline' | 'error'

export interface ServiceCardView {
  /** Service id, or "other" for robots of no configured service. */
  id: string
  enabled: boolean
  available: boolean
  robots: number
  byStatus: Record<RobotGroup, number>
  tasksToday: number
  activeTasks: number
  metrics: { key: 'completedToday' | 'roundsToday' | 'areaM2Today'; value: number }[]
}

export interface OperationsOverview {
  robots: { total: number; online: number; available: number; avgBattery: number | null }
  tasks: { active: number; executing: number; scheduled: number; completedToday: number; failedToday: number; successRate: number | null }
  attention: AlertCounts
  alerts: OpsAlert[]
  services: ServiceCardView[]
  schedule: ScheduleItem[]
  /** Admins only. */
  health?: HealthItem[]
}

export interface TaskListView {
  items: FleetTask[]
  counts: Record<TaskState, number>
  groups: Record<TaskGroup, number>
  summary: FleetTaskList['summary']
  unassigned: TaskRef[]
  late: TaskRef[]
}

export interface TaskActivityEntry {
  at: number
  type: string
  detail: string
}

export interface TaskDetail extends FleetTask {
  activity: TaskActivityEntry[]
}

// Schedule

export type ScheduleStatus = 'completed' | 'in_progress' | 'scheduled' | 'pending' | 'failed' | 'cancelled'

export interface ScheduleItem {
  type: 'task' | 'maintenance' | 'charging'
  id: string
  at: number
  end: number | null
  status: ScheduleStatus
  service: string | null
  robot: string | null
  zones: string[]
  taskId?: number
  maintenanceId?: number
  title?: string
  repeat?: Repeat
  pickupId?: string
  dropoffId?: string
  areaId?: string | null
  routeId?: string | null
}

export interface ScheduleView {
  from: number
  to: number
  items: ScheduleItem[]
}

export interface ActivityView {
  range: 'today' | 'week' | 'month'
  unit: 'hour' | 'day'
  buckets: { start: number; created: number; running: number; completed: number; failed: number }[]
}

// Maintenance

export type MaintenanceItemStatus = 'planned' | 'in_progress' | 'done' | 'cancelled'

export interface MaintenanceItem {
  id: number
  robot: string
  title: string
  status: MaintenanceItemStatus
  dueAt: number | null
  windowStart: number | null
  windowEnd: number | null
  note: string
  createdBy: string | null
  createdAt: number
  doneAt: number | null
  doneBy: string | null
}

export type MaintenanceStatus = 'none' | 'scheduled' | 'due_soon' | 'due' | 'in_progress'

export interface MaintenanceRow {
  robot: string
  serviceType: string | null
  health: RobotHealth
  status: MaintenanceStatus
  next: MaintenanceItem | null
  openItems: number
  operatingHours: number | null
  issue: string | null
}

export interface MaintenanceView {
  robots: MaintenanceRow[]
  items: MaintenanceItem[]
}

// Search

export interface SearchResults {
  robots: { name: string; fleet: string; serviceType: string | null; status: RobotStatus }[]
  tasks: { id: number; service: string; state: TaskState; pickup: LocationRef; dropoff: LocationRef; area: NamedRef | null; route: NamedRef | null }[]
  locations: Location[]
  zones: { id: string; name: I18nText }[]
  users: { id: string; fullName: string; email: string; role: Role }[]
}

// Integrations and settings (admin)

export type IntegrationId = 'open_rmf' | 'ros2_gateway' | 'vda5050' | 'mqtt' | 'vendor'

export interface IntegrationCard {
  id: IntegrationId
  status: HealthStatus | 'not_configured'
  lastUpdateAt: number | null
  fleets: string[]
  config: Record<string, unknown>
  logs: { at: number; level: AlertSeverity; code: string; params: Record<string, unknown>; closedAt: number | null }[]
  adapters?: AdapterSystem[]
}

export interface SystemSettings {
  settings: Record<string, Record<string, unknown>>
  services: (Omit<ServiceDef, 'allowed' | 'available' | 'fleets'> & { configured: boolean })[]
  zones: (Zone & { configured: boolean })[]
  fleets: Record<string, { service: string | null; capabilities: string[] }>
  templates: DeliveryTemplate[]
  sources: Record<string, string>
}
