import { Eye, Lock, Map as MapIcon, PencilRuler } from 'lucide-react'
import { useCallback, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { useLevels } from '../../api/queries'
import { Card, CardHeader } from '../../components/ui/Card'
import { Skeleton } from '../../components/ui/States'
import { Tabs } from '../../components/ui/Tabs'
import { DiscardDialog } from '../../components/ui/Unsaved'
import { usePermission } from '../auth/guards'
import { LaneLegend, LanesMap } from '../operations/LanesMap'
import { NavGraphEditor } from '../operations/NavGraphEditor'
import { cn } from '../../lib/cn'
import { useLocalize } from '../../lib/i18nText'

type Mode = 'view' | 'lanes' | 'edit'

/** Maps: viewing by default; lane closures and nav graph editing only after choosing them. */
export function MapsPanel() {
  const { t } = useTranslation()
  const localize = useLocalize()
  const levels = useLevels().data
  const canEdit = usePermission('locations.manage')
  const [levelId, setLevelId] = useState<string>()
  const [mode, setMode] = useState<Mode>('view')
  const [dirty, setDirty] = useState(false)
  const [pending, setPending] = useState<Mode | null>(null)
  const onDirtyChange = useCallback((value: boolean) => setDirty(value), [])
  const level = levels?.find((l) => l.id === levelId) ?? levels?.[0]
  const choose = (next: Mode) => (dirty && mode === 'edit' && next !== 'edit' ? setPending(next) : setMode(next))
  const modes: { value: Mode; label: string; icon: typeof Eye }[] = [
    { value: 'view', label: t('admin.maps.view'), icon: Eye },
    ...(canEdit ? [{ value: 'lanes' as Mode, label: t('ops.map.lanes'), icon: Lock }, { value: 'edit' as Mode, label: t('admin.maps.edit'), icon: PencilRuler }] : []),
  ]
  const mapClass = 'max-h-[70dvh]'

  return (
    <>
      {!level ? <Skeleton className="h-[60dvh] rounded-2xl" /> : (
        <Card className="flex flex-col gap-3 p-3 sm:p-4">
          <div className="flex flex-wrap items-center gap-3 px-1">
            <CardHeader icon={<MapIcon />} title={localize(level.label)} subtitle={t(`admin.maps.modeHint.${mode}`)} className="min-w-48 flex-1" />
            {levels && levels.length > 1 && (
              <div className="flex gap-1">
                {levels.map((l) => (
                  <button key={l.id} type="button" aria-pressed={l.id === level.id} disabled={dirty} onClick={() => setLevelId(l.id)}
                    className={cn('h-9 rounded-lg px-3 text-sm font-semibold disabled:opacity-50', l.id === level.id ? 'bg-brand-600 text-white' : 'bg-slate-100 text-slate-700')}>
                    {localize(l.label)}
                  </button>
                ))}
              </div>
            )}
            <Tabs<Mode> value={mode} onChange={choose} label={t('admin.maps.title')} className="w-full sm:w-auto"
              items={modes.map((m) => ({ value: m.value, label: m.label, icon: <m.icon className="size-4" /> }))} />
          </div>
          {mode === 'view' && <><LanesMap level={level} editable={false} className={mapClass} /><LaneLegend /></>}
          {mode === 'lanes' && <><LanesMap level={level} editable className={mapClass} /><LaneLegend /></>}
          {mode === 'edit' && <NavGraphEditor level={level} className={mapClass} onDirtyChange={onDirtyChange} />}
        </Card>
      )}
      <DiscardDialog open={pending !== null} onClose={() => setPending(null)} onDiscard={() => { setDirty(false); setMode(pending ?? 'view') }} />
    </>
  )
}
