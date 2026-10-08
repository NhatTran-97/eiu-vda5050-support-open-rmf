import { ClipboardList, Search } from 'lucide-react'
import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import { useAudit } from '../../api/queries'
import { Card, CardHeader } from '../../components/ui/Card'
import { Input, Select } from '../../components/ui/Field'
import { EmptyState, ErrorState, Skeleton } from '../../components/ui/States'
import { useAuditText } from '../admin/Audit'
import { useFormat } from '../../lib/format'

/** Who changed what and when: accounts, robots, registration, lanes, the nav graph, alerts, tasks. */
export function AuditPanel() {
  const { t, i18n } = useTranslation()
  const format = useFormat()
  const [q, setQ] = useState('')
  const [action, setAction] = useState('')
  const audit = useAudit(q, action)
  const text = useAuditText()
  const head = 'px-3 py-2.5 text-left text-xs font-semibold tracking-wide text-slate-600 uppercase'
  const actionName = (a: string) => (i18n.exists(`admin.auditAction.${a}`) ? t(`admin.auditAction.${a}`) : a)

  return (
    <>
      <Card className="p-4 sm:p-5">
        <CardHeader icon={<ClipboardList />} title={t('admin.audit.entries')} className="mb-4" />
        <div className="mb-3 flex flex-wrap gap-2">
          <div className="relative min-w-56 flex-1">
            <Search className="pointer-events-none absolute top-1/2 left-3.5 size-4 -translate-y-1/2 text-slate-400" />
            <Input value={q} onChange={(e) => setQ(e.target.value)} placeholder={t('admin.audit.search')} aria-label={t('admin.audit.search')} className="h-10 pl-10" />
          </div>
          <div className="w-full sm:w-64">
            <Select value={action} onChange={(e) => setAction(e.target.value)} aria-label={t('admin.audit.action')} compact className="w-full">
              <option value="">{t('admin.audit.allActions')}</option>
              {(audit.data?.actions ?? []).map((a) => <option key={a} value={a}>{actionName(a)}</option>)}
            </Select>
          </div>
        </div>
        {audit.isPending && <Skeleton className="h-48" />}
        {audit.isError && <ErrorState onRetry={() => void audit.refetch()} />}
        {audit.data?.items.length === 0 && <EmptyState icon={<ClipboardList />} title={t('admin.audit.empty')} />}
        {audit.data && audit.data.items.length > 0 && (
          <div className="-mx-4 overflow-x-auto sm:mx-0">
            <table className="w-full min-w-[760px] text-sm">
              <thead className="border-b border-slate-200"><tr>
                <th className={head}>{t('admin.audit.time')}</th><th className={head}>{t('admin.audit.actor')}</th>
                <th className={head}>{t('admin.audit.action')}</th><th className={head}>{t('admin.audit.target')}</th><th className={head}>{t('admin.audit.change')}</th>
              </tr></thead>
              <tbody className="divide-y divide-slate-100">
                {audit.data.items.map((e) => {
                  const x = text(e)
                  return (
                    <tr key={e.id} className="align-top hover:bg-slate-50">
                      <td className="px-3 py-2.5 whitespace-nowrap text-slate-600">{format.dateTime(e.at)}</td>
                      <td className="px-3 py-2.5 font-medium text-slate-900">{e.actor ?? t('admin.audit.system')}</td>
                      <td className="px-3 py-2.5 font-medium text-slate-800" title={e.action}>{x.action}</td>
                      <td className="px-3 py-2.5 font-mono text-meta break-all text-slate-600">{e.target || '—'}</td>
                      <td className="max-w-96 px-3 py-2.5 break-words text-slate-600">{x.detail || '—'}</td>
                    </tr>
                  )
                })}
              </tbody>
            </table>
          </div>
        )}
      </Card>
    </>
  )
}
