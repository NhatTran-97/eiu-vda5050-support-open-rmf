// Permission catalog of the demo backend (backend/eiu_web_backend/security.py); no database import, the seed uses it.
import type { OperatorPermission, Permission } from '../api/types'

export const OPERATOR_PERMISSIONS: Record<'task' | 'fleet' | 'insight', OperatorPermission[]> = {
  task: ['task.create', 'task.cancel', 'task.schedule', 'task.pause', 'task.reassign'],
  fleet: ['fleet.view', 'fleet.view_all', 'fleet.assign', 'fleet.control', 'alerts.ack'],
  insight: ['analytics.view', 'maintenance.view'],
}
export const ADMIN_PERMISSIONS: Permission[] = ['users.manage', 'robots.manage', 'locations.manage', 'maintenance.manage',
  'integrations.manage', 'settings.manage', 'system.diagnostics']
export const GRANTABLE = Object.values(OPERATOR_PERMISSIONS).flat()
export const DEFAULT_OPERATOR: OperatorPermission[] = ['task.create', 'task.cancel', 'task.schedule', 'fleet.view', 'alerts.ack',
  'analytics.view', 'maintenance.view']
