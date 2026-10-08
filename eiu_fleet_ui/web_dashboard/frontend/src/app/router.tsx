import { lazy } from 'react'
import { createBrowserRouter, Navigate, Outlet, useParams } from 'react-router'
import { RequireAdmin, RequireAuth, RequirePermission } from '../features/auth/guards'
import { AppShell } from '../layout/AppShell'
import LoginPage from '../pages/LoginPage'
import NotFoundPage from '../pages/NotFoundPage'
import NotificationsPage from '../pages/NotificationsPage'
import OverviewPage from '../pages/OverviewPage'
import { RealtimeProvider } from '../realtime/RealtimeProvider'

const LiveOperationsPage = lazy(() => import('../pages/LiveOperationsPage'))
const TasksPage = lazy(() => import('../pages/TasksPage'))
const FleetPage = lazy(() => import('../pages/FleetPage'))
const RobotPage = lazy(() => import('../pages/RobotPage'))
const SchedulePage = lazy(() => import('../pages/SchedulePage'))
const MaintenancePage = lazy(() => import('../pages/MaintenancePage'))
const AnalyticsPage = lazy(() => import('../pages/AnalyticsPage'))
const AccountPage = lazy(() => import('../pages/AccountPage'))
const MenuPage = lazy(() => import('../pages/MenuPage'))
const UsersAccessPage = lazy(() => import('../pages/admin/UsersAccessPage'))
const LocationsPage = lazy(() => import('../pages/admin/LocationsPage'))
const IntegrationsPage = lazy(() => import('../pages/admin/IntegrationsPage'))
const SystemSettingsPage = lazy(() => import('../pages/admin/SystemSettingsPage'))

/** Links of the first release keep working. */
function TaskLink() {
  const { id } = useParams()
  return <Navigate to={id ? `/tasks?task=${id}` : '/tasks'} replace />
}

function RobotLink() {
  const { name } = useParams()
  return <Navigate to={name ? `/fleet/${encodeURIComponent(name)}` : '/fleet'} replace />
}

export const router = createBrowserRouter([
  { path: '/login', element: <LoginPage /> },
  {
    element: (
      <RequireAuth>
        <RealtimeProvider>
          <Outlet />
        </RealtimeProvider>
      </RequireAuth>
    ),
    children: [
      {
        element: <AppShell />,
        children: [
          { index: true, element: <Navigate to="/overview" replace /> },
          { path: 'overview', element: <OverviewPage /> },
          { path: 'live-operations', element: <LiveOperationsPage /> },
          { path: 'tasks', element: <TasksPage /> },
          { path: 'fleet', element: <RequirePermission permission="fleet.view"><FleetPage /></RequirePermission> },
          { path: 'fleet/:name', element: <RequirePermission permission="fleet.view"><RobotPage /></RequirePermission> },
          { path: 'schedule', element: <SchedulePage /> },
          { path: 'maintenance', element: <RequirePermission permission="maintenance.view"><MaintenancePage /></RequirePermission> },
          { path: 'analytics', element: <RequirePermission permission="analytics.view"><AnalyticsPage /></RequirePermission> },
          { path: 'notifications', element: <NotificationsPage /> },
          { path: 'account', element: <AccountPage /> },
          { path: 'menu', element: <MenuPage /> },
          {
            path: 'admin',
            element: <RequireAdmin><Outlet /></RequireAdmin>,
            children: [
              { index: true, element: <Navigate to="/overview" replace /> },
              { path: 'users', element: <UsersAccessPage /> },
              { path: 'locations', element: <LocationsPage /> },
              { path: 'integrations', element: <IntegrationsPage /> },
              { path: 'settings', element: <SystemSettingsPage /> },
              { path: 'robots', element: <Navigate to="/fleet" replace /> },
              { path: 'robots/:name', element: <RobotLink /> },
              { path: 'fleets', element: <Navigate to="/fleet" replace /> },
              { path: 'maps', element: <Navigate to="/admin/locations" replace /> },
              { path: 'infrastructure', element: <Navigate to="/admin/locations" replace /> },
              { path: 'roles', element: <Navigate to="/admin/users" replace /> },
              { path: 'audit', element: <Navigate to="/admin/settings" replace /> },
              { path: 'analytics', element: <Navigate to="/analytics" replace /> },
              { path: 'diagnostics/*', element: <Navigate to="/tasks" replace /> },
              { path: '*', element: <NotFoundPage /> },
            ],
          },
          { path: 'deliveries', element: <TaskLink /> },
          { path: 'deliveries/:id', element: <TaskLink /> },
          { path: 'tracking', element: <Navigate to="/live-operations" replace /> },
          { path: 'tracking/:id', element: <TaskLink /> },
          { path: 'operations/*', element: <Navigate to="/live-operations" replace /> },
          { path: 'settings', element: <Navigate to="/account" replace /> },
          { path: '*', element: <NotFoundPage /> },
        ],
      },
    ],
  },
])
