// Authorization helpers of the UI. They only hide what the user may not use; the backend enforces every rule.
import { useCallback, useMemo } from 'react'
import { useMe } from '../api/queries'
import type { Me, Permission } from '../api/types'

export function useCurrentUser(): Me | null {
  return useMe().data ?? null
}

export function useIsAdmin(): boolean {
  return useCurrentUser()?.role === 'admin'
}

export function usePermissions(): ReadonlySet<Permission> {
  const permissions = useCurrentUser()?.permissions
  return useMemo(() => new Set(permissions ?? []), [permissions])
}

/** Whether the user holds a permission; an admin holds all of them. */
export function useCan(permission: Permission): boolean {
  return usePermissions().has(permission)
}

/** Service ids the user may operate; all enabled services for an admin. */
export function useAllowedServices(): string[] {
  const services = useCurrentUser()?.allowedServices
  return useMemo(() => services ?? [], [services])
}

export function useCanAccessService(service: string): boolean {
  return useAllowedServices().includes(service)
}

/** Whether the user sees robots and alerts of every service. */
export function useSeesAllServices(): boolean {
  const me = useCurrentUser()
  return me?.role === 'admin' || (me?.permissions.includes('fleet.view_all') ?? false)
}

/** Whether a zone is open to the user. */
export function useZoneAllowed() {
  const me = useCurrentUser()
  return useCallback((zone: string) => !me || me.allZones || me.allowedZones.includes(zone), [me])
}
