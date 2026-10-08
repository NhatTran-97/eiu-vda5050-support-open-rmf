import { useEffect, type ReactNode } from 'react'
import { Navigate, useLocation } from 'react-router'
import { useMe } from '../../api/queries'
import type { Permission } from '../../api/types'
import { ErrorState, Spinner } from '../../components/ui/States'
import { useCan, useCanAccessService, useIsAdmin } from '../../domain/access'
import i18n from '../../i18n'
import ForbiddenPage from '../../pages/ForbiddenPage'

/** A same-site path to return to after sign-in, or "/". */
export function safeNext(value: string | null): string {
  return value && value.startsWith('/') && !value.startsWith('//') && !value.startsWith('/\\') ? value : '/'
}

function FullPage({ children }: { children: ReactNode }) {
  return <div className="app-bg flex min-h-dvh items-center justify-center p-6">{children}</div>
}

/** Renders its children for a signed-in user, else sends the browser to /login. */
export function RequireAuth({ children }: { children: ReactNode }) {
  const me = useMe()
  const location = useLocation()

  useEffect(() => {
    if (me.data && i18n.language !== me.data.locale) void i18n.changeLanguage(me.data.locale)
  }, [me.data])

  if (me.isPending) return <FullPage><Spinner className="size-8 text-white" /></FullPage>
  if (me.isError) return <FullPage><div className="rounded-2xl bg-surface"><ErrorState onRetry={() => void me.refetch()} /></div></FullPage>
  if (!me.data) {
    const next = location.pathname === '/' || location.pathname === '/overview' ? '' : `?next=${encodeURIComponent(location.pathname + location.search)}`
    return <Navigate to={`/login${next}`} replace />
  }
  return children
}

export function usePermission(permission: Permission): boolean {
  return useCan(permission)
}

/** Renders its children when the user holds the permission; otherwise `fallback` (nothing by default). */
export function PermissionGuard({ permission, children, fallback = null }: { permission: Permission; children: ReactNode; fallback?: ReactNode }) {
  return useCan(permission) ? children : fallback
}

/** Renders its children when the user may operate the service. */
export function ServicePermissionGuard({ service, children, fallback = null }: { service: string; children: ReactNode; fallback?: ReactNode }) {
  return useCanAccessService(service) ? children : fallback
}

/** Route guard: the page when the user holds the permission, else the 403 page. */
export function RequirePermission({ permission, children }: { permission: Permission; children: ReactNode }) {
  return <PermissionGuard permission={permission} fallback={<ForbiddenPage />}>{children}</PermissionGuard>
}

/** Route guard of /admin/*: only the admin role. */
export function RequireAdmin({ children }: { children: ReactNode }) {
  return useIsAdmin() ? children : <ForbiddenPage />
}
