import { TriangleAlert } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { useIsAdmin } from '../domain/access'
import { useLive } from '../realtime/store'

/** Strip under the top bar while the robot fleets cannot be reached; admins also read which link is down. */
export function RmfBanner() {
  const { t } = useTranslation()
  const status = useLive((s) => s.status)
  const rmf = useLive((s) => s.rmf)
  const admin = useIsAdmin()
  if (status !== 'open' || !rmf || rmf === 'online') return null
  return (
    <div role="status" className="flex items-center gap-2 bg-amber-400 px-4 py-2 text-sm font-semibold text-amber-950 sm:px-6 lg:px-8">
      <TriangleAlert className="size-4 shrink-0" aria-hidden />
      {t('system.fleetUnavailable')}
      {admin && <span className="font-normal">· {t(`topbar.rmf.${rmf}`)}</span>}
    </div>
  )
}
