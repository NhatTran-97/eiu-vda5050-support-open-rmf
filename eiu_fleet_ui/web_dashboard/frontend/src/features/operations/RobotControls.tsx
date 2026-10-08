import { Battery, Bot, Crosshair, Gauge, MoreHorizontal, Pause, Play, Settings2, Trash2 } from 'lucide-react'
import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import { Link } from 'react-router'
import { useLevelGraph, useRegister, useRobotCommand } from '../../api/queries'
import type { FleetRobot, RobotAction } from '../../api/types'
import { Button, IconButton } from '../../components/ui/Button'
import { Menu, MenuItem, MenuSeparator } from '../../components/ui/Menu'
import { Dialog } from '../../components/ui/Dialog'
import { Field, Input, Select } from '../../components/ui/Field'
import { usePermission } from '../auth/guards'
import { cn } from '../../lib/cn'
import { useErrorText } from '../../lib/errors'
import { toast } from '../../lib/toast'

const activityTone: Record<FleetRobot['activity'], string> = {
  idle: 'bg-slate-100 text-slate-700',
  moving: 'bg-teal-50 text-teal-700',
  waiting: 'bg-amber-50 text-amber-700',
  offline: 'bg-red-50 text-red-700',
}

export function ActivityChip({ activity }: { activity: FleetRobot['activity'] }) {
  const { t } = useTranslation()
  return <span className={cn('rounded-full px-2.5 py-1 text-xs font-semibold', activityTone[activity])}>{t(`ops.activity.${activity}`)}</span>
}

type Open = 'pause' | 'speed-limit' | 'init-position' | 'remove' | null

const modeTone: Record<string, string> = {
  emergency: 'bg-red-50 text-red-700',
  adapter_error: 'bg-red-50 text-red-700',
  offline: 'bg-red-50 text-red-700',
  charging: 'bg-emerald-50 text-emerald-700',
  paused: 'bg-amber-50 text-amber-700',
  waiting: 'bg-amber-50 text-amber-700',
}

/** RMF mode of the robot, or "paused" when the last pause through the gateway is not resumed. */
export function ModeChip({ robot }: { robot: FleetRobot }) {
  const { t, i18n } = useTranslation()
  const mode = robot.paused && robot.mode !== 'offline' ? 'paused' : robot.mode
  const known = i18n.exists(`ops.mode.${mode}`)
  return (
    <span className={cn('rounded-full px-2.5 py-1 text-xs font-semibold whitespace-nowrap', modeTone[mode] ?? activityTone[robot.activity])}>
      {known ? t(`ops.mode.${mode}`) : t(`ops.activity.${robot.activity}`)}
    </span>
  )
}

function speedText(robot: FleetRobot, t: (k: string) => string) {
  if (robot.speedLimit === null) return t('ops.robots.speedUnknown')
  return robot.speedLimit > 0 ? `${robot.speedLimit} m/s` : t('ops.robots.speedNone')
}

/** Operator actions of one robot: Pause or Resume as the main action, the rest in a menu; dangerous ones last. */
export function RobotActions({ robot, compact, onConfigure }: { robot: FleetRobot; compact?: boolean; onConfigure?: () => void }) {
  const { t } = useTranslation()
  const canControl = usePermission('fleet.control')
  const canRegister = usePermission('robots.manage')
  const command = useRobotCommand()
  const errorText = useErrorText()
  const [open, setOpen] = useState<Open>(null)
  const controls = canControl && robot.controls

  const run = (action: RobotAction, body?: Record<string, unknown>) => command.mutate({ robot: robot.name, action, body }, {
    onSuccess: () => {
      toast.success(t('ops.robots.done', { robot: robot.name, action: t(`ops.robots.${actionLabel[action]}`) }))
      setOpen(null)
    },
    onError: (e) => toast.error(errorText(e)),
  })
  const pauseButton = (
    <Button size="sm" variant="secondary" icon={<Pause className="size-4" />} disabled={!controls} onClick={() => setOpen('pause')}>
      {t('ops.robots.pause')}
    </Button>
  )
  const resumeButton = (
    <Button size="sm" variant={robot.paused ? 'primary' : 'secondary'} icon={<Play className="size-4" />} disabled={!controls}
      loading={command.isPending && command.variables?.action === 'resume'} onClick={() => run('resume')}>
      {t('ops.robots.resume')}
    </Button>
  )

  return (
    <div className={cn('flex items-center gap-2', compact ? 'justify-end' : 'flex-wrap')}>
      {robot.paused === true ? resumeButton : pauseButton}
      {robot.paused === null && resumeButton}
      {!compact && (
        <Button size="sm" variant="ghost" icon={<Gauge className="size-4" />} disabled={!controls} onClick={() => setOpen('speed-limit')}>
          {t('ops.robots.speedLimit')}
        </Button>
      )}
      {compact && onConfigure && (
        <Button size="sm" variant="ghost" icon={<Settings2 className="size-4" />} onClick={onConfigure}>{t('admin.robots.configure')}</Button>
      )}
      <Menu
        trigger={({ toggle, open: menuOpen }) => (
          <IconButton label={t('common.moreActions')} onClick={toggle} aria-expanded={menuOpen} className="size-9">
            <MoreHorizontal className="size-5" />
          </IconButton>
        )}
      >
        {(close) => (
          <>
            {onConfigure && !compact && <MenuItem onClick={() => { close(); onConfigure() }}><Settings2 />{t('admin.robots.configure')}</MenuItem>}
            {robot.paused === true && <MenuItem onClick={() => { close(); setOpen('pause') }}><Pause />{t('ops.robots.pause')}</MenuItem>}
            {robot.paused === false && <MenuItem onClick={() => { close(); run('resume') }}><Play />{t('ops.robots.resume')}</MenuItem>}
            {compact && controls && <MenuItem onClick={() => { close(); setOpen('speed-limit') }}><Gauge />{t('ops.robots.speedLimit')}</MenuItem>}
            {controls && <MenuItem onClick={() => { close(); setOpen('init-position') }}><Crosshair />{t('ops.robots.setPosition')}</MenuItem>}
            {canRegister && (
              <>
                <MenuSeparator />
                <MenuItem danger onClick={() => { close(); setOpen('remove') }}><Trash2 />{t('ops.robots.remove')}</MenuItem>
              </>
            )}
          </>
        )}
      </Menu>

      <Dialog
        open={open === 'pause'}
        onClose={() => setOpen(null)}
        title={t('ops.robots.pauseTitle', { robot: robot.name })}
        footer={<>
          <Button variant="secondary" onClick={() => setOpen(null)}>{t('common.cancel')}</Button>
          <Button loading={command.isPending} onClick={() => run('pause')}>{t('ops.robots.pause')}</Button>
        </>}
      >
        <p className="text-sm text-slate-600">{t('ops.robots.pauseBody')}</p>
      </Dialog>
      {open === 'speed-limit' && <SpeedDialog robot={robot} pending={command.isPending} onClose={() => setOpen(null)} onApply={(mps) => run('speed-limit', { mps })} />}
      {open === 'init-position' && <PositionDialog robot={robot} pending={command.isPending} onClose={() => setOpen(null)} onApply={(body) => run('init-position', body)} />}
      {open === 'remove' && <RemoveDialog robot={robot} onClose={() => setOpen(null)} />}
    </div>
  )
}

/** One robot of the fleet with its operator actions. */
export function RobotCard({ robot, onConfigure }: { robot: FleetRobot; onConfigure?: () => void }) {
  const { t } = useTranslation()
  return (
    <li className="flex min-w-0 flex-col gap-3 rounded-xl p-4 ring-1 ring-slate-200">
      <div className="flex items-start gap-3">
        <span className="flex size-10 shrink-0 items-center justify-center rounded-full bg-brand-50 text-brand-600"><Bot className="size-5" /></span>
        <div className="min-w-0 flex-1">
          <p className="truncate font-bold text-slate-900">{robot.name}</p>
          <p className="truncate text-sm text-slate-500">{robot.fleet}{robot.adapter ? ` · ${robot.adapter}` : ''}</p>
        </div>
        <ModeChip robot={robot} />
      </div>
      <dl className="grid grid-cols-3 gap-2 text-sm">
        <div>
          <dt className="text-slate-500">{t('ops.robots.battery')}</dt>
          <dd className="flex items-center gap-1 font-semibold text-slate-900"><Battery className="size-4 text-slate-400" />{Math.round(robot.battery)}%</dd>
        </div>
        <div>
          <dt className="text-slate-500">{t('ops.robots.speedShort')}</dt>
          <dd className="font-semibold text-slate-900">{speedText(robot, t)}</dd>
        </div>
        <div className="min-w-0">
          <dt className="text-slate-500">{t('ops.robots.task')}</dt>
          <dd className="truncate font-semibold text-slate-900" title={robot.taskId ?? ''}>
            {robot.deliveryId ? `#${robot.deliveryId}` : robot.taskId ?? t('ops.robots.noTask')}
          </dd>
        </div>
      </dl>
      {!robot.controls && <p className="text-xs text-slate-500">{t('ops.robots.noControls')}</p>}
      <RobotActions robot={robot} onConfigure={onConfigure} />
    </li>
  )
}

/** Configuration facts of a robot from its fleet's registry. */
export interface RobotInfo {
  series?: string
  interface?: string
  manufacturer?: string
  serial?: string
  charger?: string
  source?: string
}

/** Configuration table for a large fleet. */
export function RobotTable({ robots, info = {}, onConfigure }: {
  robots: FleetRobot[]
  info?: Record<string, RobotInfo>
  onConfigure?: (robot: FleetRobot) => void
}) {
  const { t } = useTranslation()
  const head = 'px-3 py-2.5 text-left text-xs font-semibold tracking-wide text-slate-500 uppercase'
  return (
    <div className="-mx-4 overflow-x-auto sm:mx-0">
      <table className="w-full min-w-[960px] text-sm">
        <thead className="border-b border-slate-200">
          <tr>
            <th className={head}>{t('ops.robots.robot')}</th>
            <th className={head}>{t('admin.robots.type')}</th>
            <th className={head}>{t('ops.robots.fleet')}</th>
            <th className={head}>{t('admin.robots.interface')}</th>
            <th className={head}>{t('admin.robots.map')}</th>
            <th className={head}>{t('ops.robots.status')}</th>
            <th className={head}>{t('ops.robots.battery')}</th>
            <th className={head}>{t('admin.robots.charger')}</th>
            <th className={cn(head, 'text-right')}>{t('ops.robots.actions')}</th>
          </tr>
        </thead>
        <tbody className="divide-y divide-slate-100">
          {robots.map((r) => (
            <tr key={r.name} className="hover:bg-slate-50">
              <td className="px-3 py-2.5 font-semibold text-slate-900">
                <Link to={`/fleet/${encodeURIComponent(r.name)}`} className="hover:underline">{r.name}</Link>
              </td>
              <td className="max-w-36 truncate px-3 py-2.5 text-slate-600">{info[r.name]?.series ?? '—'}</td>
              <td className="max-w-40 truncate px-3 py-2.5 text-slate-600" title={r.adapter ?? ''}>{r.fleet}</td>
              <td className="px-3 py-2.5 text-slate-600">{info[r.name]?.interface ? `VDA5050 · ${info[r.name].interface}` : '—'}</td>
              <td className="px-3 py-2.5 text-slate-600">{r.levelId || '—'}</td>
              <td className="px-3 py-2.5"><ModeChip robot={r} /></td>
              <td className={cn('px-3 py-2.5 font-medium', r.battery < 20 ? 'text-red-600' : 'text-slate-700')}>{Math.round(r.battery)}%</td>
              <td className="px-3 py-2.5 text-slate-600">{info[r.name]?.charger || '—'}</td>
              <td className="px-3 py-2"><RobotActions robot={r} compact onConfigure={onConfigure && (() => onConfigure(r))} /></td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  )
}

const actionLabel: Record<RobotAction, string> = {
  pause: 'pause',
  resume: 'resume',
  'speed-limit': 'speedLimit',
  'init-position': 'setPosition',
}

export function SpeedDialog({ robot, pending, onClose, onApply }: { robot: FleetRobot; pending: boolean; onClose: () => void; onApply: (mps: number) => void }) {
  const { t } = useTranslation()
  const [value, setValue] = useState(String(robot.speedLimit ?? 0))
  const mps = Number(value)
  const valid = value.trim() !== '' && Number.isFinite(mps) && mps >= 0
  return (
    <Dialog
      open
      onClose={onClose}
      title={t('ops.robots.speedTitle', { robot: robot.name })}
      footer={<>
        <Button variant="secondary" onClick={onClose}>{t('common.cancel')}</Button>
        <Button disabled={!valid} loading={pending} onClick={() => onApply(mps)}>{t('ops.robots.apply')}</Button>
      </>}
    >
      <Field label={t('ops.robots.speedLabel')} htmlFor="speed-limit" hint={t('ops.robots.speedHint')}>
        <Input id="speed-limit" type="number" inputMode="decimal" min={0} step={0.05} value={value} onChange={(e) => setValue(e.target.value)} />
      </Field>
    </Dialog>
  )
}

export function PositionDialog({ robot, pending, onClose, onApply }: {
  robot: FleetRobot
  pending: boolean
  onClose: () => void
  onApply: (body: { waypoint: string; yaw: number }) => void
}) {
  const { t } = useTranslation()
  const graph = useLevelGraph(robot.levelId).data
  const named = (graph?.vertices ?? []).filter((v) => v.name)
  const nearest = named.reduce<{ name: string; d: number } | null>((best, v) => {
    const d = Math.hypot(v.x - robot.x, v.y - robot.y)
    return !best || d < best.d ? { name: v.name, d } : best
  }, null)
  const [waypoint, setWaypoint] = useState<string>()
  const [yawDeg, setYawDeg] = useState(String(Math.round((robot.yaw * 180) / Math.PI)))
  const chosen = waypoint ?? nearest?.name ?? ''
  const yaw = Number(yawDeg)
  const valid = !!chosen && yawDeg.trim() !== '' && Number.isFinite(yaw)
  return (
    <Dialog
      open
      onClose={onClose}
      title={t('ops.robots.positionTitle', { robot: robot.name })}
      footer={<>
        <Button variant="secondary" onClick={onClose}>{t('common.cancel')}</Button>
        <Button disabled={!valid} loading={pending} onClick={() => onApply({ waypoint: chosen, yaw: (yaw * Math.PI) / 180 })}>{t('ops.robots.apply')}</Button>
      </>}
    >
      <div className="flex flex-col gap-4">
        <Field label={t('ops.robots.waypoint')} htmlFor="init-waypoint" hint={t('ops.robots.positionHint')}>
          <Select id="init-waypoint" value={chosen} onChange={(e) => setWaypoint(e.target.value)}>
            {named.map((v) => <option key={v.name} value={v.name}>{v.name}</option>)}
          </Select>
        </Field>
        <Field label={t('ops.robots.yaw')} htmlFor="init-yaw">
          <Input id="init-yaw" type="number" inputMode="decimal" step={5} value={yawDeg} onChange={(e) => setYawDeg(e.target.value)} />
        </Field>
      </div>
    </Dialog>
  )
}

/** Removal asks for the robot's name, so it cannot be confirmed by a stray click. */
export function RemoveDialog({ robot, onClose, onRemoved }: { robot: FleetRobot; onClose: () => void; onRemoved?: () => void }) {
  const { t } = useTranslation()
  const register = useRegister()
  const errorText = useErrorText()
  const [typed, setTyped] = useState('')
  return (
    <Dialog
      open
      onClose={onClose}
      title={t('ops.robots.removeTitle', { robot: robot.name, fleet: robot.fleet })}
      footer={<>
        <Button variant="secondary" onClick={onClose}>{t('common.cancel')}</Button>
        <Button
          variant="danger"
          disabled={typed.trim() !== robot.name}
          loading={register.isPending}
          onClick={() => register.mutate({ action: 'remove', fleet: robot.fleet, name: robot.name }, {
            onSuccess: (result) => {
              if (result.ok) {
                toast.success(t('ops.register.removed', { name: robot.name }))
                onClose()
                onRemoved?.()
              } else {
                toast.error(result.errors.map((e) => e.message).join('; ') || t('ops.register.failed'))
              }
            },
            onError: (e) => toast.error(errorText(e)),
          })}
        >
          {t('ops.robots.remove')}
        </Button>
      </>}
    >
      <div className="flex flex-col gap-4">
        <p className="text-sm text-slate-600">{t('ops.robots.removeBody')}</p>
        <Field label={t('ops.robots.removeType', { robot: robot.name })} htmlFor="remove-confirm">
          <Input id="remove-confirm" value={typed} autoComplete="off" spellCheck={false} onChange={(e) => setTyped(e.target.value)} placeholder={robot.name} />
        </Field>
      </div>
    </Dialog>
  )
}
