import type { LucideIcon } from 'lucide-react'
import type { ReactNode } from 'react'
import { Kpi } from '../../features/overview/ops'

const TONE = { brand: 'blue', warning: 'amber', critical: 'red', success: 'green' } as const

/** KPI tile of a page: the compact Overview KPI. A `warning` or `critical` tone marks a value that needs action. */
export function StatCard({ icon, label, value, line, tone = 'brand', to, loading }: {
  icon: LucideIcon
  label: string
  value: ReactNode
  line?: ReactNode
  tone?: 'brand' | 'warning' | 'critical' | 'success'
  to?: string
  loading?: boolean
}) {
  const alert = tone === 'warning' ? 'amber' : tone === 'critical' ? 'red' : undefined
  return <Kpi compact icon={icon} tone={TONE[tone]} label={label} value={value} line={line} to={to} loading={loading} alert={alert} />
}
