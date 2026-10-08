import { ArrowLeft, Lock, Plus } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { useServices } from '../../api/queries'
import { Button } from '../../components/ui/Button'
import { Dialog } from '../../components/ui/Dialog'
import { EmptyState, Skeleton } from '../../components/ui/States'
import { useCan } from '../../domain/access'
import { serviceIcons } from '../../domain/services'
import { useLocalize } from '../../lib/i18nText'
import { toast } from '../../lib/toast'
import { useUi } from '../../lib/ui'
import { DynamicTaskForm } from './DynamicTaskForm'
import { taskCode } from './taskText'

/** "+ Create Task": pick one of the user's services, then fill that service's form. */
/** Where "Create Task" shows: in the top bar from `sm`, in the page header below it, so a screen never has two. */
export const createTaskInTopBar = 'hidden sm:block'
export const createTaskInHeader = 'sm:hidden'

export function CreateTaskButton({ className }: { className?: string }) {
  const { t } = useTranslation()
  const canCreate = useCan('task.create')
  const open = useUi((s) => s.openCreateTask)
  if (!canCreate) return null
  return <Button icon={<Plus className="size-4" />} onClick={() => open()} className={className}>{t('tasks.create')}</Button>
}

export function CreateTaskDialog() {
  const { t } = useTranslation()
  const localize = useLocalize()
  const state = useUi((s) => s.createTask)
  const close = useUi((s) => s.closeCreateTask)
  const choose = useUi((s) => s.openCreateTask)
  const openTask = useUi((s) => s.openTask)
  const services = useServices()
  const usable = (services.data ?? []).filter((s) => s.allowed && s.enabled)
  const service = usable.find((s) => s.id === state.service)

  return (
    <Dialog open={state.open} onClose={close} className="max-w-2xl"
      title={service ? (
        <span className="flex items-center gap-2">
          <button type="button" aria-label={t('common.back')} onClick={() => choose(null)} className="rounded-lg p-1 text-slate-500 hover:bg-slate-100"><ArrowLeft className="size-5" /></button>
          {t('tasks.createService', { service: localize(service.name) })}
        </span>
      ) : t('tasks.createTitle')}>
      {!service && (
        <div>
          <p className="mb-3 text-sm font-semibold text-slate-700">{t('tasks.selectService')}</p>
          {services.isPending && <Skeleton className="h-32" />}
          {services.data && usable.length === 0 && (
            <EmptyState icon={<Lock />} title={t('empty.noServices.title')} body={t('empty.noServices.body')} />
          )}
          <div className="grid gap-3 sm:grid-cols-2">
            {usable.map((s) => {
              const Icon = serviceIcons[s.icon]
              return (
                <button key={s.id} type="button" onClick={() => choose(s.id)}
                  className="flex items-start gap-3 rounded-2xl p-4 text-left ring-1 ring-slate-200 transition hover:bg-brand-50 hover:ring-brand-300">
                  <span className="flex size-11 shrink-0 items-center justify-center rounded-xl bg-brand-50 text-brand-600"><Icon className="size-5" /></span>
                  <span className="min-w-0">
                    <span className="block font-bold text-slate-900">{localize(s.name)}</span>
                    <span className="block text-sm text-slate-500">{localize(s.description)}</span>
                    {!s.available && <span className="mt-1 block text-xs font-medium text-amber-700">{t('tasks.noFleetShort')}</span>}
                  </span>
                </button>
              )
            })}
          </div>
        </div>
      )}
      {service && (
        <DynamicTaskForm key={service.id} service={service} onCancel={close} onCreated={(task) => {
          toast.success(t('tasks.created', { code: taskCode(task.id, task.service) }))
          close()
          openTask(task.id)
        }} />
      )}
    </Dialog>
  )
}
