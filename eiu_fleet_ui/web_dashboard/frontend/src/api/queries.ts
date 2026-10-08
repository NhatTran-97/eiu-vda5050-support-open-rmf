import { keepPreviousData, useMutation, useQuery, useQueryClient, type QueryClient } from '@tanstack/react-query'
import { ApiError, http } from './client'
import type {
  AccessCatalog,
  ActivityView,
  AdminOverview,
  AdminUser,
  AlertList,
  AnalyticsReport,
  AppNotification,
  AuditList,
  Catalog,
  DeliveryList,
  DeliveryTemplate,
  FleetRobot,
  GraphLaneEdit,
  GraphVertexEdit,
  Infrastructure,
  IntegrationCard,
  LanesView,
  Level,
  LevelGraph,
  Locale,
  Location,
  MaintenanceItem,
  MaintenanceView,
  Me,
  NavGraphView,
  NewTask,
  NotificationList,
  NotificationPrefs,
  OperationsOverview,
  RealtimeTopic,
  RegistrationRequest,
  RegistrationResult,
  RegistrationView,
  Role,
  RobotAction,
  RobotDetailView,
  ScheduleView,
  SearchResults,
  ServiceDef,
  SiteConfig,
  SystemSettings,
  SystemView,
  TaskDetail,
  TaskGroup,
  TaskListView,
  TaskState,
  UserAccessPatch,
} from './types'

export interface TaskFilters {
  service: string
  state: TaskState | ''
  q: string
  group: TaskGroup
  mine?: boolean
}

export interface ScheduleFilters {
  start: number
  end: number
  service?: string
  robot?: string
  zone?: string
}

export interface AnalyticsFilters {
  days: number
  service?: string
  robot?: string
  zone?: string
}

export const qk = {
  me: ['me'] as const,
  config: ['config'] as const,
  levels: ['levels'] as const,
  graph: (levelId: string) => ['levels', levelId, 'graph'] as const,
  locations: ['locations'] as const,
  templates: ['templates'] as const,
  services: ['services'] as const,
  catalog: ['catalog'] as const,
  deliveries: ['deliveries'] as const,
  tasks: ['tasks'] as const,
  taskList: (f: TaskFilters) => ['tasks', 'list', f] as const,
  task: (id: number) => ['tasks', 'detail', id] as const,
  overview: ['tasks', 'overview'] as const,
  schedule: (f: ScheduleFilters) => ['tasks', 'schedule', f] as const,
  activity: (range: string, service: string) => ['tasks', 'activity', range, service] as const,
  notifications: ['notifications'] as const,
  fleet: ['fleet'] as const,
  fleetRobots: ['fleet', 'robots'] as const,
  robotDetail: (name: string, hours: number) => ['fleet', 'robot', name, hours] as const,
  registration: ['fleet', 'registration'] as const,
  lanes: ['fleet', 'lanes'] as const,
  navGraph: (levelId: string) => ['fleet', 'nav-graph', levelId] as const,
  system: ['fleet', 'system'] as const,
  maintenance: ['fleet', 'maintenance'] as const,
  alerts: (state: string) => ['alerts', state] as const,
  search: (q: string) => ['search', q] as const,
  analytics: (f: AnalyticsFilters) => ['analytics', f] as const,
  admin: ['admin'] as const,
  adminOverview: ['admin', 'overview'] as const,
  infrastructure: ['admin', 'infrastructure'] as const,
  integrations: ['admin', 'integrations'] as const,
  settings: ['admin', 'settings'] as const,
  audit: (q: string, action: string) => ['admin', 'audit', q, action] as const,
  adminUsers: ['admin', 'users'] as const,
  accessCatalog: ['admin', 'access-catalog'] as const,
}

/** Query keys refreshed when the realtime stream reports a change of a topic. */
export function invalidateTopic(client: QueryClient, topic: RealtimeTopic) {
  if (topic === 'deliveries') {
    void client.invalidateQueries({ queryKey: qk.deliveries })
    void client.invalidateQueries({ queryKey: qk.tasks })
  } else if (topic === 'alerts') {
    void client.invalidateQueries({ queryKey: ['alerts'] })
    void client.invalidateQueries({ queryKey: qk.overview })
  } else if (topic === 'fleet') {
    void client.invalidateQueries({ queryKey: qk.fleet })
    void client.invalidateQueries({ queryKey: qk.tasks })
    void client.invalidateQueries({ queryKey: qk.admin })
    void client.invalidateQueries({ queryKey: ['levels'] })
    void client.invalidateQueries({ queryKey: qk.locations })
  } else if (topic === 'access') {
    // The account's services, zones or permissions changed: everything it sees may differ.
    void client.invalidateQueries()
  } else {
    void client.invalidateQueries({ queryKey: qk.notifications })
  }
}

// Session

export function useMe() {
  return useQuery({
    queryKey: qk.me,
    queryFn: async () => {
      try {
        return await http.get<Me>('/auth/me')
      } catch (e) {
        if (e instanceof ApiError && e.status === 401) return null
        throw e
      }
    },
    staleTime: 60_000,
  })
}

export function useLogin() {
  const client = useQueryClient()
  return useMutation({
    mutationFn: (body: { email: string; password: string }) => http.post<Me>('/auth/login', body),
    onSuccess: (me) => client.setQueryData(qk.me, me),
  })
}

export function useLogout() {
  const client = useQueryClient()
  return useMutation({
    mutationFn: () => http.post<void>('/auth/logout'),
    onSettled: () => {
      client.clear()
      client.setQueryData(qk.me, null)
    },
  })
}

export function useUpdatePreferences() {
  const client = useQueryClient()
  return useMutation({
    mutationFn: (body: Partial<{ locale: Locale; notificationPrefs: NotificationPrefs }>) =>
      http.put<Me>('/me/preferences', body),
    onSuccess: (me) => client.setQueryData(qk.me, me),
  })
}

export function useChangePassword() {
  return useMutation({
    mutationFn: (body: { currentPassword: string; newPassword: string }) => http.post<void>('/me/password', body),
  })
}

// Site data and services

export function useConfig() {
  return useQuery({ queryKey: qk.config, queryFn: () => http.get<SiteConfig>('/config'), staleTime: Infinity })
}

export function useLevels() {
  return useQuery({ queryKey: qk.levels, queryFn: () => http.get<Level[]>('/levels'), staleTime: Infinity })
}

export function useLevelGraph(levelId: string | undefined) {
  return useQuery({
    queryKey: qk.graph(levelId ?? ''),
    queryFn: () => http.get<LevelGraph>(`/levels/${encodeURIComponent(levelId!)}/graph`),
    enabled: !!levelId,
    staleTime: Infinity,
  })
}

export function useLocations() {
  return useQuery({ queryKey: qk.locations, queryFn: () => http.get<Location[]>('/locations'), staleTime: 10 * 60_000 })
}

export function useTemplates() {
  return useQuery({ queryKey: qk.templates, queryFn: () => http.get<DeliveryTemplate[]>('/templates'), staleTime: 10 * 60_000 })
}

export function useServices() {
  return useQuery({ queryKey: qk.services, queryFn: () => http.get<ServiceDef[]>('/services'), staleTime: 60_000 })
}

export function useCatalog() {
  return useQuery({ queryKey: qk.catalog, queryFn: () => http.get<Catalog>('/catalog'), staleTime: 10 * 60_000 })
}

export function useSearch(q: string) {
  return useQuery({
    queryKey: qk.search(q),
    queryFn: () => http.get<SearchResults>(`/search?q=${encodeURIComponent(q)}`),
    enabled: q.trim().length > 0,
    placeholderData: keepPreviousData,
    staleTime: 10_000,
  })
}

// Overview, tasks, schedule

export function useOperationsOverview() {
  return useQuery({ queryKey: qk.overview, queryFn: () => http.get<OperationsOverview>('/overview'), refetchInterval: 10_000 })
}

export function useTasks(filters: TaskFilters) {
  const params = new URLSearchParams({ service_type: filters.service, state: filters.state, q: filters.q, group: filters.group })
  if (filters.mine) params.set('mine', 'true')
  return useQuery({
    queryKey: qk.taskList(filters),
    queryFn: () => http.get<TaskListView>(`/tasks?${params}`),
    placeholderData: keepPreviousData,
  })
}

export function useTask(id: number | undefined) {
  return useQuery({
    queryKey: qk.task(id ?? -1),
    queryFn: () => http.get<TaskDetail>(`/tasks/${id}`),
    enabled: id !== undefined && Number.isFinite(id),
  })
}

export function useMyTasks() {
  return useQuery({ queryKey: [...qk.deliveries, 'all'], queryFn: () => http.get<DeliveryList>('/deliveries?group=all') })
}

export function useCreateTask() {
  const client = useQueryClient()
  return useMutation({
    mutationFn: (body: NewTask) => http.post<TaskDetail>('/tasks', body),
    onSuccess: () => invalidateTopic(client, 'deliveries'),
  })
}

export type TaskAction = 'cancel' | 'pause' | 'resume' | 'reassign'

export function useTaskAction() {
  const client = useQueryClient()
  return useMutation({
    mutationFn: ({ id, action, robot }: { id: number; action: TaskAction; robot?: string }) =>
      http.post<TaskDetail>(`/tasks/${id}/${action}`, robot ? { robot } : {}),
    onSuccess: (task) => {
      client.setQueryData(qk.task(task.id), task)
      invalidateTopic(client, 'deliveries')
      void client.invalidateQueries({ queryKey: qk.fleet })
    },
  })
}

export function useSchedule(filters: ScheduleFilters) {
  const params = new URLSearchParams({ start: String(filters.start), end: String(filters.end), service_type: filters.service ?? '',
    robot: filters.robot ?? '', zone: filters.zone ?? '' })
  return useQuery({
    queryKey: qk.schedule(filters),
    queryFn: () => http.get<ScheduleView>(`/schedule?${params}`),
    placeholderData: keepPreviousData,
    refetchInterval: 30_000,
  })
}

export function useActivity(range: 'today' | 'week' | 'month', service: string) {
  return useQuery({
    queryKey: qk.activity(range, service),
    queryFn: () => http.get<ActivityView>(`/analytics/activity?range=${range}&service_type=${encodeURIComponent(service)}`),
    placeholderData: keepPreviousData,
    refetchInterval: 30_000,
  })
}

export function useAnalytics(filters: AnalyticsFilters, enabled = true) {
  const params = new URLSearchParams({ days: String(filters.days), service_type: filters.service ?? '', robot: filters.robot ?? '',
    zone: filters.zone ?? '' })
  return useQuery({
    queryKey: qk.analytics(filters),
    queryFn: () => http.get<AnalyticsReport>(`/analytics?${params}`),
    placeholderData: keepPreviousData,
    enabled,
  })
}

// Notifications and alerts

export function useNotifications() {
  return useQuery({ queryKey: qk.notifications, queryFn: () => http.get<NotificationList>('/notifications') })
}

export function useMarkNotificationsRead() {
  const client = useQueryClient()
  return useMutation({
    mutationFn: async (id: number | 'all') => {
      if (id === 'all') await http.post<void>('/notifications/read-all')
      else await http.post<AppNotification>(`/notifications/${id}/read`)
    },
    onSuccess: () => invalidateTopic(client, 'notifications'),
  })
}

export function useAlerts(state: 'open' | 'all', enabled = true) {
  return useQuery({ queryKey: qk.alerts(state), queryFn: () => http.get<AlertList>(`/alerts?state=${state}`), enabled })
}

export function useAlertAction() {
  const client = useQueryClient()
  return useMutation({
    mutationFn: ({ id, action }: { id: number | 'all'; action: 'ack' | 'resolve' }) =>
      id === 'all' ? http.post<{ acknowledged: number }>('/fleet/alerts/ack-all') : http.post<unknown>(`/fleet/alerts/${id}/${action}`),
    onSettled: () => {
      void client.invalidateQueries({ queryKey: ['alerts'] })
      void client.invalidateQueries({ queryKey: qk.overview })
    },
  })
}

export function useResetDemo() {
  const client = useQueryClient()
  return useMutation({
    mutationFn: () => http.post<void>('/demo/reset'),
    onSuccess: () => client.invalidateQueries(),
  })
}

// Fleet

export function useFleetRobots(enabled = true) {
  return useQuery({ queryKey: qk.fleetRobots, queryFn: () => http.get<FleetRobot[]>('/fleet/robots'), refetchInterval: 5000, enabled })
}

export function useRobotDetail(name: string | undefined, hours: number) {
  return useQuery({
    queryKey: qk.robotDetail(name ?? '', hours),
    queryFn: () => http.get<RobotDetailView>(`/fleet/robots/${encodeURIComponent(name!)}?hours=${hours}`),
    enabled: !!name,
    refetchInterval: 5000,
    placeholderData: keepPreviousData,
  })
}

export function useRobotCommand() {
  const client = useQueryClient()
  return useMutation({
    mutationFn: ({ robot, action, body }: { robot: string; action: RobotAction; body?: Record<string, unknown> }) =>
      http.post<{ ok: boolean; message: string }>(`/fleet/robots/${encodeURIComponent(robot)}/${action}`, body ?? {}),
    onSettled: () => client.invalidateQueries({ queryKey: qk.fleet }),
  })
}

export function useRegistration(enabled = true) {
  return useQuery({ queryKey: qk.registration, queryFn: () => http.get<RegistrationView>('/fleet/registration'), enabled })
}

export function useRegister() {
  const client = useQueryClient()
  return useMutation({
    mutationFn: (body: RegistrationRequest) => http.post<RegistrationResult>('/fleet/registration', body),
    onSuccess: (result, body) => {
      if (body.action !== 'check' && result.ok) void client.invalidateQueries({ queryKey: qk.fleet })
    },
  })
}

export function useLanes() {
  return useQuery({ queryKey: qk.lanes, queryFn: () => http.get<LanesView>('/fleet/lanes') })
}

export function useSetLanes() {
  const client = useQueryClient()
  return useMutation({
    mutationFn: (body: { close?: number[]; open?: number[]; fleet?: string }) =>
      http.post<{ ok: boolean; fleets: string[] }>('/fleet/lanes', body),
    onSettled: () => client.invalidateQueries({ queryKey: qk.lanes }),
  })
}

export function useNavGraph(levelId: string | undefined) {
  return useQuery({
    queryKey: qk.navGraph(levelId ?? ''),
    queryFn: () => http.get<NavGraphView>(`/fleet/nav-graph${levelId ? `?level=${encodeURIComponent(levelId)}` : ''}`),
  })
}

export function useSaveNavGraph() {
  const client = useQueryClient()
  return useMutation({
    mutationFn: (body: { levelId: string; baseSha256: string; vertices: GraphVertexEdit[]; lanes: GraphLaneEdit[] }) =>
      http.put<{ ok: boolean; sha256: string }>('/fleet/nav-graph', body),
    onSuccess: () => {
      void client.invalidateQueries({ queryKey: qk.fleet })
      void client.invalidateQueries({ queryKey: ['levels'] })
      void client.invalidateQueries({ queryKey: qk.locations })
    },
  })
}

export function useSystem(enabled = true) {
  return useQuery({ queryKey: qk.system, queryFn: () => http.get<SystemView>('/fleet/system'), refetchInterval: 10_000, enabled })
}

export function useMaintenance(enabled = true) {
  return useQuery({ queryKey: qk.maintenance, queryFn: () => http.get<MaintenanceView>('/maintenance'), refetchInterval: 30_000, enabled })
}

export function useSaveMaintenance() {
  const client = useQueryClient()
  return useMutation({
    mutationFn: ({ id, ...body }: Partial<MaintenanceItem> & { id?: number }) =>
      id === undefined ? http.post<MaintenanceItem>('/maintenance', body) : http.patch<MaintenanceItem>(`/maintenance/${id}`, body),
    onSettled: () => {
      void client.invalidateQueries({ queryKey: qk.fleet })
      void client.invalidateQueries({ queryKey: qk.tasks })
    },
  })
}

// Administration

export function useAdminOverview(enabled = true) {
  return useQuery({ queryKey: qk.adminOverview, queryFn: () => http.get<AdminOverview>('/admin/overview'), refetchInterval: 10_000, enabled })
}

export function useInfrastructure() {
  return useQuery({ queryKey: qk.infrastructure, queryFn: () => http.get<Infrastructure>('/admin/infrastructure') })
}

export function useIntegrations() {
  return useQuery({ queryKey: qk.integrations, queryFn: () => http.get<IntegrationCard[]>('/admin/integrations'), refetchInterval: 5000 })
}

export function useSystemSettings() {
  return useQuery({ queryKey: qk.settings, queryFn: () => http.get<SystemSettings>('/admin/settings') })
}

export function useToggle() {
  const client = useQueryClient()
  return useMutation({
    mutationFn: ({ kind, id, enabled }: { kind: 'service' | 'zone'; id: string; enabled: boolean }) =>
      http.put<{ enabled: boolean }>(`/admin/toggles/${kind}/${encodeURIComponent(id)}`, { enabled }),
    onSettled: () => client.invalidateQueries(),
  })
}

export function useAudit(q: string, action: string) {
  const params = new URLSearchParams({ q, action })
  return useQuery({ queryKey: qk.audit(q, action), queryFn: () => http.get<AuditList>(`/admin/audit?${params}`), placeholderData: keepPreviousData })
}

export function useAdminUsers(enabled = true) {
  return useQuery({ queryKey: qk.adminUsers, queryFn: () => http.get<AdminUser[]>('/admin/users'), enabled })
}

export function useAccessCatalog(enabled = true) {
  return useQuery({ queryKey: qk.accessCatalog, queryFn: () => http.get<AccessCatalog>('/admin/access-catalog'), enabled, staleTime: 60_000 })
}

export function useCreateUser() {
  const client = useQueryClient()
  return useMutation({
    mutationFn: (body: UserAccessPatch & { email: string; fullName: string; role: Role; password: string; locale?: Locale }) =>
      http.post<AdminUser>('/admin/users', body),
    onSettled: () => client.invalidateQueries({ queryKey: qk.admin }),
  })
}

export function useUpdateUser() {
  const client = useQueryClient()
  return useMutation({
    mutationFn: ({ id, ...body }: UserAccessPatch & { id: string }) => http.patch<AdminUser>(`/admin/users/${id}`, body),
    onSettled: () => {
      void client.invalidateQueries({ queryKey: qk.admin })
      void client.invalidateQueries({ queryKey: qk.me })
    },
  })
}

export function useSetUserPassword() {
  const client = useQueryClient()
  return useMutation({
    mutationFn: ({ id, password }: { id: string; password: string }) => http.post<void>(`/admin/users/${id}/password`, { password }),
    onSettled: () => client.invalidateQueries({ queryKey: qk.admin }),
  })
}
