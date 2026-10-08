import { ChevronDown, Crosshair, Pause, Play, Trash2 } from 'lucide-react'
import { useState, type ReactNode } from 'react'
import { useTranslation } from 'react-i18next'
import { useRobotCommand } from '../../api/queries'
import type { FleetRegistry, FleetRobot, RobotAction } from '../../api/types'
import { Button } from '../../components/ui/Button'
import { Dialog } from '../../components/ui/Dialog'
import { Input } from '../../components/ui/Field'
import { useErrorText } from '../../lib/errors'
import { toast } from '../../lib/toast'
import { usePermission } from '../auth/guards'
import { ModeChip, PositionDialog, RemoveDialog, type RobotInfo } from '../operations/RobotControls'

function Section({ title, open, children, danger }: { title: string; open?: boolean; children: ReactNode; danger?: boolean }) {
  return (
    <details open={open} className="group rounded-xl ring-1 ring-slate-200 open:bg-slate-50/60">
      <summary className={`flex cursor-pointer list-none items-center gap-2 px-4 py-3 font-semibold ${danger ? 'text-red-700' : 'text-slate-900'}`}>
        <span className="flex-1">{title}</span>
        <ChevronDown className="size-4 text-slate-400 transition-transform group-open:rotate-180" />
      </summary>
      <div className="px-4 pb-4">{children}</div>
    </details>
  )
}

function Facts({ rows }: { rows: [string, ReactNode][] }) {
  return (
    <dl className="grid grid-cols-[minmax(8rem,auto)_1fr] gap-x-6 gap-y-2 text-sm">
      {rows.map(([label, value]) => (
        <div key={label} className="contents">
          <dt className="text-slate-500">{label}</dt>
          <dd className="break-words text-slate-900">{value ?? '—'}</dd>
        </div>
      ))}
    </dl>
  )
}

/** Configuration of one robot, section by section; values defined in the fleet adapter's config are read-only here. */
export function RobotDetail({ robot, info, fleet, onClose }: {
  robot: FleetRobot
  info: RobotInfo
  fleet?: FleetRegistry
  onClose: () => void
}) {
  const { t } = useTranslation()
  const canControl = usePermission('fleet.control') && robot.controls
  const canRegister = usePermission('robots.manage')
  const command = useRobotCommand()
  const errorText = useErrorText()
  const [speed, setSpeed] = useState(String(robot.speedLimit ?? 0))
  const [dialog, setDialog] = useState<'position' | 'remove' | null>(null)
  const mps = Number(speed)
  const speedValid = speed.trim() !== '' && Number.isFinite(mps) && mps >= 0
  const speedChanged = speedValid && mps !== (robot.speedLimit ?? 0)

  const run = (action: RobotAction, body?: Record<string, unknown>, done?: () => void) => command.mutate({ robot: robot.name, action, body }, {
    onSuccess: () => {
      toast.success(t('admin.robots.saved', { robot: robot.name }))
      done?.()
    },
    onError: (e) => toast.error(errorText(e)),
  })
  const limits = fleet?.limits ?? {}

  return (
    <Dialog open onClose={onClose} className="max-w-2xl" title={
      <span className="flex flex-wrap items-center gap-3">{t('admin.robots.detailTitle', { robot: robot.name })}<ModeChip robot={robot} /></span>
    }>
      <div className="flex flex-col gap-3">
        <p className="text-sm text-slate-600">{t('admin.robots.detailHint')}</p>
        <Section title={t('admin.robots.general')} open>
          <Facts rows={[
            [t('ops.robots.robot'), robot.name],
            [t('ops.robots.fleet'), robot.fleet],
            [t('admin.robots.type'), info.series],
            [t('admin.robots.manufacturer'), info.manufacturer],
            [t('admin.robots.serial'), info.serial],
            [t('admin.robots.source'), info.source ? t(`admin.robots.sourceKind.${info.source}`, { defaultValue: info.source }) : undefined],
          ]} />
        </Section>
        <Section title={t('admin.robots.connectivity')}>
          <Facts rows={[
            [t('admin.robots.interface'), info.interface ? `VDA5050 · ${info.interface}` : undefined],
            [t('admin.robots.adapterNode'), robot.adapter],
            [t('admin.robots.controls'), t(robot.controls ? 'admin.yes' : 'admin.no')],
          ]} />
        </Section>
        <Section title={t('admin.robots.navigation')} open>
          <Facts rows={[
            [t('admin.robots.map'), robot.levelId],
            [t('admin.robots.position'), `x ${robot.x.toFixed(2)} · y ${robot.y.toFixed(2)} · ${Math.round((robot.yaw * 180) / Math.PI)}°`],
            [t('admin.robots.charger'), info.charger],
            [t('admin.robots.fleetSpeed'), limits.linear_speed !== undefined ? `${limits.linear_speed} m/s` : undefined],
          ]} />
          <div className="mt-4 flex flex-wrap items-end gap-2">
            <label className="min-w-40 flex-1 text-sm">
              <span className="mb-1.5 block font-semibold text-slate-800">{t('ops.robots.speedLabel')}</span>
              <Input type="number" inputMode="decimal" min={0} step={0.05} value={speed} disabled={!canControl} onChange={(e) => setSpeed(e.target.value)} className="h-11" />
            </label>
            <Button disabled={!canControl || !speedChanged} loading={command.isPending && command.variables?.action === 'speed-limit'}
              onClick={() => run('speed-limit', { mps })}>{t('common.save')}</Button>
          </div>
          <p className="mt-1.5 text-xs text-slate-500">{speedChanged ? t('admin.unsaved') : t('ops.robots.speedHint')}</p>
        </Section>
        <Section title={t('admin.robots.operations')}>
          <div className="flex flex-wrap gap-2">
            {robot.paused === true
              ? <Button size="sm" icon={<Play className="size-4" />} disabled={!canControl} onClick={() => run('resume')}>{t('ops.robots.resume')}</Button>
              : <Button size="sm" variant="secondary" icon={<Pause className="size-4" />} disabled={!canControl} onClick={() => run('pause')}>{t('ops.robots.pause')}</Button>}
            {robot.paused === null && <Button size="sm" variant="secondary" icon={<Play className="size-4" />} disabled={!canControl} onClick={() => run('resume')}>{t('ops.robots.resume')}</Button>}
            <Button size="sm" variant="ghost" icon={<Crosshair className="size-4" />} disabled={!canControl} onClick={() => setDialog('position')}>{t('ops.robots.setPosition')}</Button>
          </div>
        </Section>
        {canRegister && (
          <Section title={t('admin.robots.danger')} danger>
            <p className="mb-3 text-sm text-slate-600">{t('ops.robots.removeBody')}</p>
            <Button size="sm" variant="danger" icon={<Trash2 className="size-4" />} onClick={() => setDialog('remove')}>{t('ops.robots.remove')}</Button>
          </Section>
        )}
      </div>
      {dialog === 'position' && <PositionDialog robot={robot} pending={command.isPending} onClose={() => setDialog(null)} onApply={(body) => run('init-position', body, () => setDialog(null))} />}
      {dialog === 'remove' && <RemoveDialog robot={robot} onClose={() => setDialog(null)} onRemoved={onClose} />}
    </Dialog>
  )
}
