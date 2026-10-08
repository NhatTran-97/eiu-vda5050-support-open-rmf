import { Bot, CircleAlert, CircleCheck, FileText, Footprints, MapPin, Navigation, OctagonX, PackageCheck, TriangleAlert, X, type LucideIcon } from 'lucide-react'
import { useCallback } from 'react'
import { useTranslation } from 'react-i18next'
import type { AppNotification, NotificationType } from '../../api/types'
import { useLocalize } from '../../lib/i18nText'

export const notificationStyle: Record<NotificationType, { icon: LucideIcon; tone: string }> = {
  request_received: { icon: FileText, tone: 'bg-slate-100 text-slate-600' },
  robot_at_pickup: { icon: Bot, tone: 'bg-amber-50 text-amber-600' },
  package_loaded: { icon: PackageCheck, tone: 'bg-emerald-50 text-emerald-600' },
  near_destination: { icon: MapPin, tone: 'bg-teal-50 text-teal-600' },
  robot_arrived: { icon: Navigation, tone: 'bg-amber-50 text-amber-600' },
  delivered: { icon: CircleCheck, tone: 'bg-emerald-50 text-emerald-600' },
  cancelled: { icon: X, tone: 'bg-slate-100 text-slate-500' },
  delayed: { icon: TriangleAlert, tone: 'bg-amber-50 text-amber-500' },
  action_required: { icon: CircleAlert, tone: 'bg-red-50 text-red-500' },
  patrol_started: { icon: Footprints, tone: 'bg-teal-50 text-teal-600' },
  patrol_completed: { icon: CircleCheck, tone: 'bg-emerald-50 text-emerald-600' },
  task_failed: { icon: OctagonX, tone: 'bg-red-50 text-red-500' },
}

/** What a notification asks of the reader: act now, a problem to know about, or an update of a delivery. */
export type NotificationCategory = 'action' | 'issues' | 'deliveries'

export const notificationCategory: Record<NotificationType, NotificationCategory> = {
  robot_at_pickup: 'action',
  robot_arrived: 'action',
  action_required: 'action',
  delayed: 'issues',
  task_failed: 'issues',
  cancelled: 'issues',
  request_received: 'deliveries',
  package_loaded: 'deliveries',
  near_destination: 'deliveries',
  delivered: 'deliveries',
  patrol_started: 'deliveries',
  patrol_completed: 'deliveries',
}

/** The task a notification is about; its drawer opens on click. */
export function notificationTarget(n: AppNotification): { taskId: number } | null {
  return n.deliveryId === null ? null : { taskId: n.deliveryId }
}

/** Title and body of a notification in the current language; params stored per language are resolved first. */
export function useNotificationText() {
  const { t } = useTranslation()
  const localize = useLocalize()
  return useCallback((n: AppNotification) => {
    const params = Object.fromEntries(Object.entries(n.params).map(([k, v]) => [k, localize(v)]))
    return {
      title: t(`notifications.types.${n.type}.title`, params),
      body: t(`notifications.types.${n.type}.body`, params),
    }
  }, [t, localize])
}
