import { Save, TriangleAlert, Undo2 } from 'lucide-react'
import { useEffect } from 'react'
import { useTranslation } from 'react-i18next'
import { useBlocker } from 'react-router'
import { Button } from './Button'
import { Dialog } from './Dialog'

/** Bar shown while an editor holds changes that are not saved. */
export function UnsavedBar({ onDiscard, onSave, saving }: { onDiscard: () => void; onSave: () => void; saving?: boolean }) {
  const { t } = useTranslation()
  return (
    <div role="status" className="flex flex-wrap items-center gap-3 rounded-xl bg-amber-50 px-4 py-2.5 text-sm font-semibold text-amber-800 ring-1 ring-amber-200">
      <TriangleAlert className="size-4" aria-hidden />
      <span className="flex-1">{t('admin.unsaved')}</span>
      <Button size="sm" variant="ghost" icon={<Undo2 className="size-4" />} onClick={onDiscard}>{t('admin.discard')}</Button>
      <Button size="sm" icon={<Save className="size-4" />} loading={saving} onClick={onSave}>{t('admin.saveChanges')}</Button>
    </div>
  )
}

/** Asks before the page is left with unsaved changes: links inside the app and closing or reloading the tab. */
export function LeaveGuard({ dirty }: { dirty: boolean }) {
  const { t } = useTranslation()
  const blocker = useBlocker(({ currentLocation, nextLocation }) => dirty && currentLocation.pathname !== nextLocation.pathname)

  useEffect(() => {
    if (!dirty) return
    const warn = (e: BeforeUnloadEvent) => e.preventDefault()
    window.addEventListener('beforeunload', warn)
    return () => window.removeEventListener('beforeunload', warn)
  }, [dirty])

  return (
    <Dialog
      open={blocker.state === 'blocked'}
      onClose={() => blocker.reset?.()}
      title={t('admin.leaveTitle')}
      footer={<>
        <Button variant="secondary" onClick={() => blocker.reset?.()}>{t('admin.stay')}</Button>
        <Button variant="danger" onClick={() => blocker.proceed?.()}>{t('admin.leave')}</Button>
      </>}
    >
      <p className="text-sm text-slate-600">{t('admin.leaveBody')}</p>
    </Dialog>
  )
}

/** Confirmation before throwing the changes away. */
export function DiscardDialog({ open, onClose, onDiscard }: { open: boolean; onClose: () => void; onDiscard: () => void }) {
  const { t } = useTranslation()
  return (
    <Dialog
      open={open}
      onClose={onClose}
      title={t('admin.discardTitle')}
      footer={<>
        <Button variant="secondary" onClick={onClose}>{t('admin.stay')}</Button>
        <Button variant="danger" onClick={() => { onDiscard(); onClose() }}>{t('admin.discard')}</Button>
      </>}
    >
      <p className="text-sm text-slate-600">{t('admin.discardBody')}</p>
    </Dialog>
  )
}
