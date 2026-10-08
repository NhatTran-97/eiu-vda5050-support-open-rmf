import { useCallback } from 'react'
import { useCatalog, useLocations } from '../../api/queries'
import type { Delivery, LiveTask, ScheduleItem } from '../../api/types'
import { useLocalize } from '../../lib/i18nText'

type TaskLike = Pick<LiveTask, 'kind' | 'pickupId' | 'dropoffId' | 'areaId' | 'routeId'> & { stops?: string[] }

/** One line naming where a task goes: "Library → Room 204", the cleaning area, or the patrol route. */
export function useTaskPlace() {
  const localize = useLocalize()
  const locations = useLocations().data
  const catalog = useCatalog().data
  const place = useCallback((id: string | null | undefined) => {
    if (!id) return ''
    const loc = locations?.find((l) => l.id === id)
    return loc ? localize(loc.name) : id
  }, [locations, localize])
  const text = useCallback((task: TaskLike): string => {
    if (task.kind === 'clean') {
      const area = catalog?.areas.find((a) => a.id === task.areaId)
      return area ? localize(area.name) : task.areaId ?? ''
    }
    if (task.kind === 'patrol') {
      const route = catalog?.routes.find((r) => r.id === task.routeId)
      if (route) return localize(route.name)
      return (task.stops ?? []).map(place).join(' → ')
    }
    return `${place(task.pickupId)} → ${place(task.dropoffId)}`
  }, [catalog, localize, place])
  return { text, place }
}

export function fromDelivery(d: Delivery): TaskLike {
  return { kind: d.kind, pickupId: d.pickup.id, dropoffId: d.dropoff.id, areaId: d.area?.id ?? null, routeId: d.route?.id ?? null,
    stops: d.stops.map((s) => s.id) }
}

export function fromSchedule(i: ScheduleItem, kind: Delivery['kind']): TaskLike {
  return { kind, pickupId: i.pickupId ?? '', dropoffId: i.dropoffId ?? '', areaId: i.areaId ?? null, routeId: i.routeId ?? null }
}

/** Task number with the service's prefix, e.g. D-1048. */
export function taskCode(id: number, service: string): string {
  return `${service.slice(0, 1).toUpperCase()}-${String(id).padStart(4, '0')}`
}
