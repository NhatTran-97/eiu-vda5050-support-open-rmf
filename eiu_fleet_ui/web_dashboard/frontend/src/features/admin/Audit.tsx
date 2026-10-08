import { useTranslation } from 'react-i18next'
import type { AuditEntry } from '../../api/types'
import { useFormat } from '../../lib/format'

function parse(detail: string): unknown {
  try {
    return JSON.parse(detail)
  } catch {
    return detail
  }
}

function flat(value: unknown): string {
  if (value === null || value === undefined) return '—'
  if (Array.isArray(value)) return value.map(flat).join(', ')
  if (typeof value === 'object') return Object.entries(value as Record<string, unknown>).map(([k, v]) => `${k}: ${flat(v)}`).join(' · ')
  return String(value)
}

/** The change an audit entry records, in words where the action is known. */
export function useAuditText() {
  const { t, i18n } = useTranslation()
  return (entry: AuditEntry) => {
    const key = `admin.auditAction.${entry.action}`
    const action = i18n.exists(key) ? t(key) : entry.action
    const detail = parse(entry.detail)
    let text = typeof detail === 'string' ? detail : flat(detail)
    if (entry.action === 'robot.speed_limit' && detail && typeof detail === 'object') {
      const d = detail as { mps?: number; previous_mps?: number | null }
      const fmt = (v: number | null | undefined) => (v === null || v === undefined ? '?' : v === 0 ? t('ops.robots.speedNone') : `${v} m/s`)
      text = `${fmt(d.previous_mps)} → ${fmt(d.mps)}`
    } else if (entry.action === 'maintenance.update' && typeof detail === 'string' && /^#\d+ [a-z_]+$/.test(detail)) {
      // The backend records "#<id> <status>" for a maintenance status change.
      const [id, status] = detail.slice(1).split(' ')
      text = t('admin.audit.maintenanceItem', { id, status: t(`admin.audit.maintenanceStatus.${status}`, { defaultValue: status }) })
    } else if (entry.action === 'lanes.request' && detail && typeof detail === 'object') {
      const d = detail as { close?: number[]; open?: number[] }
      text = [d.close?.length ? t('admin.audit.closed', { lanes: d.close.join(', ') }) : '', d.open?.length ? t('admin.audit.opened', { lanes: d.open.join(', ') }) : ''].filter(Boolean).join(' · ')
    }
    return { action, detail: text }
  }
}

export function AuditRow({ entry, compact }: { entry: AuditEntry; compact?: boolean }) {
  const { t } = useTranslation()
  const format = useFormat()
  const text = useAuditText()(entry)
  return (
    <li className="py-2.5 text-sm">
      <p className="text-slate-800">
        <span className="font-semibold text-slate-900">{entry.actor ?? t('admin.audit.system')}</span>
        {' · '}{text.action}{entry.target && <span className="font-medium"> {entry.target}</span>}
      </p>
      {text.detail && <p className={compact ? 'truncate text-slate-600' : 'break-words text-slate-600'}>{text.detail}</p>}
      <p className="text-xs text-slate-500">{format.dateTime(entry.at)}</p>
    </li>
  )
}
