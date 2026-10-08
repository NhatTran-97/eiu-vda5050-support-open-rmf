import L from 'leaflet'
import { CircleDot, MousePointer2, Spline, Trash2 } from 'lucide-react'
import { useEffect, useMemo, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { Marker, Polyline, Tooltip, useMapEvents } from 'react-leaflet'
import { useNavGraph, useSaveNavGraph } from '../../api/queries'
import type { GraphVertexEdit, Level, NavGraphView } from '../../api/types'
import { Button } from '../../components/ui/Button'
import { Field, Input } from '../../components/ui/Field'
import { ErrorState, Skeleton } from '../../components/ui/States'
import { Switch } from '../../components/ui/Switch'
import { DiscardDialog, LeaveGuard, UnsavedBar } from '../../components/ui/Unsaved'
import { cn } from '../../lib/cn'
import { useErrorText } from '../../lib/errors'
import { toast } from '../../lib/toast'
import { addLane, pairKey, removeVertex, type Draft } from './graph'
import { MapCanvas } from './MapCanvas'

type Tool = 'select' | 'vertex' | 'lane'
type Selection = { kind: 'vertex'; index: number } | { kind: 'lane'; a: number; b: number } | null

const vertexIcon = (charger: boolean, selected: boolean) => L.divIcon({
  className: '',
  html: `<div class="graph-vertex${charger ? ' graph-vertex--charger' : ''}${selected ? ' graph-vertex--selected' : ''}"></div>`,
  iconSize: [16, 16],
  iconAnchor: [8, 8],
  tooltipAnchor: [0, -6],
})

function MapClicks({ onClick }: { onClick: (latlng: L.LatLng) => void }) {
  useMapEvents({ click: (e) => onClick(e.latlng) })
  return null
}

/** Editor of one level of the nav graph file the gateway serves; saving writes the file through the gateway. */
export function NavGraphEditor({ level, className, onDirtyChange }: { level: Level; className?: string; onDirtyChange?: (dirty: boolean) => void }) {
  const { t } = useTranslation()
  const view = useNavGraph(level.id)
  if (view.isPending) return <Skeleton className={cn('rounded-xl', className)} />
  if (view.isError) return <ErrorState onRetry={() => void view.refetch()} />
  if (!view.data.available || !view.data.sha256) {
    return <p className="rounded-xl bg-amber-50 p-4 text-sm text-amber-800">{t('ops.editor.unavailable')}</p>
  }
  return <Editor key={view.data.sha256} level={level} view={view.data} className={className} onDirtyChange={onDirtyChange} />
}

function Editor({ level, view, className, onDirtyChange }: { level: Level; view: NavGraphView; className?: string; onDirtyChange?: (dirty: boolean) => void }) {
  const { t } = useTranslation()
  const save = useSaveNavGraph()
  const errorText = useErrorText()
  const original = useMemo<Draft>(() => ({ vertices: view.vertices ?? [], lanes: view.lanes ?? [] }), [view])
  const [draft, setDraft] = useState<Draft>(original)
  const dirty = useMemo(() => JSON.stringify(draft) !== JSON.stringify(original), [draft, original])
  const [tool, setTool] = useState<Tool>('select')
  const [both, setBoth] = useState(true)
  const [laneStart, setLaneStart] = useState<number | null>(null)
  const [selection, setSelection] = useState<Selection>(null)

  const [confirmDiscard, setConfirmDiscard] = useState(false)

  useEffect(() => {
    onDirtyChange?.(dirty)
  }, [dirty, onDirtyChange])
  useEffect(() => () => onDirtyChange?.(false), [onDirtyChange])

  const update = setDraft
  const pairs = useMemo(() => {
    const out = new Map<string, { a: number; b: number; forward: boolean; backward: boolean }>()
    for (const l of draft.lanes) {
      const key = pairKey(l.from, l.to)
      const a = Math.min(l.from, l.to)
      const entry = out.get(key) ?? { a, b: Math.max(l.from, l.to), forward: false, backward: false }
      if (l.from === a) entry.forward = true
      else entry.backward = true
      out.set(key, entry)
    }
    return [...out.entries()]
  }, [draft.lanes])

  const onMapClick = (p: L.LatLng) => {
    if (tool !== 'vertex') return
    const vertices = [...draft.vertices, { name: '', x: Math.round(p.lng * 1000) / 1000, y: Math.round(p.lat * 1000) / 1000, charger: false, attrs: {} }]
    update({ ...draft, vertices })
    setSelection({ kind: 'vertex', index: vertices.length - 1 })
  }
  const onVertexClick = (index: number) => {
    if (tool === 'lane') {
      if (laneStart === null) setLaneStart(index)
      else if (laneStart !== index) {
        update(addLane(draft, laneStart, index, both))
        setSelection({ kind: 'lane', a: Math.min(laneStart, index), b: Math.max(laneStart, index) })
        setLaneStart(null)
      }
      return
    }
    setSelection({ kind: 'vertex', index })
  }
  const setVertex = (index: number, patch: Partial<GraphVertexEdit>) =>
    update({ ...draft, vertices: draft.vertices.map((v, i) => (i === index ? { ...v, ...patch } : v)) })
  const chooseTool = (next: Tool) => {
    setTool(next)
    setLaneStart(null)
  }
  const discard = () => {
    setDraft(original)
    setSelection(null)
  }
  const submit = () => save.mutate(
    { levelId: view.levelId ?? level.id, baseSha256: view.sha256 ?? '', vertices: draft.vertices, lanes: draft.lanes },
    {
      onSuccess: () => {
        toast.success(t('ops.editor.saved'))
        toast.info(t('ops.editor.restartNote'))
      },
      onError: (e) => toast.error(errorText(e)),
    },
  )

  const tools: { value: Tool; icon: typeof MousePointer2; label: string }[] = [
    { value: 'select', icon: MousePointer2, label: t('ops.editor.select') },
    { value: 'vertex', icon: CircleDot, label: t('ops.editor.addVertex') },
    { value: 'lane', icon: Spline, label: t('ops.editor.addLane') },
  ]
  const hint = tool === 'vertex' ? t('ops.editor.addHint') : tool === 'lane' ? (laneStart === null ? t('ops.editor.pickFirst') : t('ops.editor.pickSecond')) : ''
  const vertex = selection?.kind === 'vertex' ? draft.vertices[selection.index] : undefined
  const lane = selection?.kind === 'lane' ? pairs.find(([key]) => key === pairKey(selection.a, selection.b))?.[1] : undefined
  const label = (i: number) => draft.vertices[i]?.name || `#${i}`

  return (
    <div className="flex flex-col gap-3">
      <div className="flex flex-wrap items-center gap-2">
        <div role="radiogroup" aria-label={t('ops.map.edit')} className="flex gap-1 rounded-xl bg-slate-100 p-1">
          {tools.map(({ value, icon: Icon, label: text }) => (
            <button
              key={value}
              type="button"
              role="radio"
              aria-checked={tool === value}
              onClick={() => chooseTool(value)}
              className={cn('flex h-9 items-center gap-1.5 rounded-lg px-3 text-sm font-semibold', tool === value ? 'bg-surface text-brand-700 shadow-sm' : 'text-slate-600 hover:text-slate-900')}
            >
              <Icon className="size-4" />{text}
            </button>
          ))}
        </div>
        {tool === 'lane' && <Switch label={t('ops.editor.bidirectional')} checked={both} onChange={setBoth} />}
        <span className="ml-auto text-sm text-slate-500">
          {t('ops.editor.summary', { vertices: draft.vertices.length, lanes: draft.lanes.length })}
        </span>
      </div>
      {dirty && <UnsavedBar onDiscard={() => setConfirmDiscard(true)} onSave={submit} saving={save.isPending} />}
      <LeaveGuard dirty={dirty} />
      <DiscardDialog open={confirmDiscard} onClose={() => setConfirmDiscard(false)} onDiscard={discard} />
      {hint && <p className="text-sm text-slate-500">{hint}</p>}
      <div className="grid gap-3 lg:grid-cols-[minmax(0,1fr)_18rem]">
        <MapCanvas level={level} showRobots={false} className={className}>
          <MapClicks onClick={onMapClick} />
          {pairs.map(([key, p]) => {
            const va = draft.vertices[p.a]
            const vb = draft.vertices[p.b]
            const chosen = selection?.kind === 'lane' && pairKey(selection.a, selection.b) === key
            return (
              <Polyline
                key={key}
                positions={[[va.y, va.x], [vb.y, vb.x]]}
                pathOptions={{
                  color: chosen ? '#f59e0b' : p.forward && p.backward ? '#475569' : '#7c3aed',
                  weight: chosen ? 7 : 4,
                  opacity: 0.85,
                  dashArray: p.forward && p.backward ? undefined : '10 6',
                  bubblingMouseEvents: false,
                }}
                eventHandlers={{ click: () => tool === 'select' && setSelection({ kind: 'lane', a: p.a, b: p.b }) }}
              />
            )
          })}
          {draft.vertices.map((v, i) => (
            <Marker
              key={i}
              position={[v.y, v.x]}
              icon={vertexIcon(v.charger, (selection?.kind === 'vertex' && selection.index === i) || laneStart === i)}
              draggable={tool === 'select'}
              keyboard={false}
              eventHandlers={{
                click: () => onVertexClick(i),
                dragend: (e) => {
                  const p = (e.target as L.Marker).getLatLng()
                  setVertex(i, { x: Math.round(p.lng * 1000) / 1000, y: Math.round(p.lat * 1000) / 1000 })
                  setSelection({ kind: 'vertex', index: i })
                },
              }}
            >
              {v.name && <Tooltip permanent direction="top" className="map-label">{v.name}</Tooltip>}
            </Marker>
          ))}
        </MapCanvas>

        <aside className="flex flex-col gap-4 rounded-xl p-4 ring-1 ring-slate-200">
          {view.path && <p className="truncate text-xs text-slate-500" title={view.path}>{t('ops.editor.file')}: {view.path}</p>}
          {!selection && <p className="text-sm text-slate-500">{t('ops.editor.selectHint')}</p>}
          {vertex && selection?.kind === 'vertex' && (
            <>
              <p className="font-bold text-slate-900">{t('ops.editor.vertex', { index: selection.index })}</p>
              <Field label={t('ops.editor.name')} htmlFor="vertex-name">
                <Input id="vertex-name" value={vertex.name} autoComplete="off" onChange={(e) => setVertex(selection.index, { name: e.target.value.trim() })} />
              </Field>
              <p className="text-sm text-slate-500">x {vertex.x.toFixed(3)} · y {vertex.y.toFixed(3)}</p>
              <Switch label={t('ops.editor.charger')} checked={vertex.charger} onChange={(charger) => setVertex(selection.index, { charger })} />
              <Button variant="danger" size="sm" icon={<Trash2 className="size-4" />} onClick={() => {
                update(removeVertex(draft, selection.index))
                setSelection(null)
              }}>
                {t('ops.editor.deleteVertex')}
              </Button>
            </>
          )}
          {lane && selection?.kind === 'lane' && (
            <>
              <p className="font-bold text-slate-900">{t('ops.editor.lane', { from: label(lane.a), to: label(lane.b) })}</p>
              <p className="text-sm text-slate-500">
                {[lane.forward && `${label(lane.a)} → ${label(lane.b)}`, lane.backward && `${label(lane.b)} → ${label(lane.a)}`].filter(Boolean).join(' · ')}
              </p>
              <Button variant="danger" size="sm" icon={<Trash2 className="size-4" />} onClick={() => {
                update({ ...draft, lanes: draft.lanes.filter((l) => pairKey(l.from, l.to) !== pairKey(lane.a, lane.b)) })
                setSelection(null)
              }}>
                {t('ops.editor.deleteLane')}
              </Button>
            </>
          )}
        </aside>
      </div>
    </div>
  )
}
