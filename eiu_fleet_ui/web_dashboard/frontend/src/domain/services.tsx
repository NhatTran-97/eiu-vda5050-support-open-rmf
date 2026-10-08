import { Bot, Camera, Package, Shield, Sparkles, Truck, Wrench, type LucideIcon } from 'lucide-react'
import { useCallback, useMemo } from 'react'
import { useTranslation } from 'react-i18next'
import { useServices } from '../api/queries'
import type { ServiceDef, ServiceIcon as ServiceIconName } from '../api/types'
import { cn } from '../lib/cn'
import { useLocalize } from '../lib/i18nText'

export const serviceIcons: Record<ServiceIconName, LucideIcon> = {
  package: Package,
  sparkles: Sparkles,
  shield: Shield,
  bot: Bot,
  truck: Truck,
  camera: Camera,
  wrench: Wrench,
}

/** Services by id, as the backend serves them to this user. */
export function useServiceMap(): Map<string, ServiceDef> {
  const services = useServices().data
  return useMemo(() => new Map((services ?? []).map((s) => [s.id, s])), [services])
}

/** Icon of a service id; robots of no configured service get the generic robot icon. */
export function useServiceIcon() {
  const map = useServiceMap()
  return useCallback((id: string | null | undefined): LucideIcon => {
    const svc = id ? map.get(id) : undefined
    return svc ? serviceIcons[svc.icon] ?? Bot : Bot
  }, [map])
}

/** Name of a service id in the current language. */
export function useServiceName() {
  const { t } = useTranslation()
  const localize = useLocalize()
  const map = useServiceMap()
  return useCallback((id: string | null | undefined): string => {
    if (!id || id === 'other') return t('service.other')
    const svc = map.get(id)
    return svc ? localize(svc.name) : t(`service.${id}`, { defaultValue: id })
  }, [map, localize, t])
}

export function ServiceIcon({ service, className }: { service: string | null | undefined; className?: string }) {
  const Icon = useServiceIcon()(service)
  return <Icon className={cn('size-4', className)} aria-hidden />
}

/** Service icon and name in one compact chip. */
export function ServiceTag({ service, className }: { service: string | null | undefined; className?: string }) {
  const name = useServiceName()
  return (
    <span className={cn('inline-flex items-center gap-1.5 text-sm text-slate-700', className)}>
      <span className="flex size-6 items-center justify-center rounded-md bg-slate-100 text-slate-600"><ServiceIcon service={service} className="size-3.5" /></span>
      {name(service)}
    </span>
  )
}
