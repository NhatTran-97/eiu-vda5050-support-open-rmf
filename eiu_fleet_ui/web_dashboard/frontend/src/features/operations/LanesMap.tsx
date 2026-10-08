import type L from 'leaflet'
import { BoxSelect as BoxIcon, Check, Layers, Lock, LockOpen, X } from 'lucide-react'
import { useMemo, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { Polyline, Tooltip, Marker } from 'react-leaflet'
import { useLanes, useLevelGraph, useSetLanes } from '../../api/queries'
import type { Level } from '../../api/types'
import { Button } from '../../components/ui/Button'
import { Menu, MenuItem } from '../../components/ui/Menu'
import { useStored } from '../../lib/useStored'
import { Skeleton } from '../../components/ui/States'
import { usePermission } from '../auth/guards'
import { cn } from '../../lib/cn'
import { useErrorText } from '../../lib/errors'
import { toast } from '../../lib/toast'
import { dotIcon } from '../map/icons'
import { corridors, segmentInBox, type Corridor } from './graph'
import { BoxSelect, MapCanvas } from './MapCanvas'

type Layer = 'robots' | 'lanes' | 'labels'
const LAYERS: Layer[] = ['robots', 'lanes', 'labels']
const isLayers = (v: unknown): v is Layer[] => Array.isArray(v) && v.every((x) => LAYERS.includes(x as Layer))

/** Choice of the map layers to draw. */
function LayerMenu({ layers, onChange }: { layers: Layer[]; onChange: (layers: Layer[]) => void }) {
  const { t } = useTranslation()
  return (
    <Menu
      align="start"
      trigger={({ toggle, open }) => (
        <Button size="sm" variant="secondary" icon={<Layers className="size-4" />} onClick={toggle} aria-expanded={open}>{t('ops.map.layers')}</Button>
      )}
    >
      {() => LAYERS.map((layer) => (
        <MenuItem key={layer} onClick={() => onChange(layers.includes(layer) ? layers.filter((l) => l !== layer) : [...layers, layer])}>
          <span className={cn('flex size-4 items-center justify-center rounded border', layers.includes(layer) ? 'border-brand-600 bg-brand-600 text-white' : 'border-slate-300')}>
            {layers.includes(layer) && <Check className="size-3" />}
          </span>
          {t(`ops.map.layer.${layer}`)}
        </MenuItem>
      ))}
    </Menu>
  )
}

const laneStyle = {
  open: { color: '#64748b', weight: 3, opacity: 0.7 },
  closed: { color: '#dc2626', weight: 4, opacity: 0.9, dashArray: '8 6' },
  selected: { color: '#f59e0b', weight: 7, opacity: 0.95 },
}

export function LaneLegend() {
  const { t } = useTranslation()
  const item = 'flex h-9 items-center gap-2 rounded-xl bg-surface px-3 text-sm font-medium text-slate-700 ring-1 ring-slate-200'
  return (
    <div className="flex flex-wrap gap-2">
      <span className={item}><span className="h-1 w-5 rounded-full bg-slate-500" />{t('ops.map.legendOpen')}</span>
      <span className={item}><span className="h-1 w-5 rounded-full border-t-4 border-dashed border-red-600" />{t('ops.map.legendClosed')}</span>
      <span className={item}><span className="h-1.5 w-5 rounded-full bg-amber-500" />{t('ops.map.legendSelected')}</span>
    </div>
  )
}

/** Floor map with lane closures; operators with map:edit close and open corridors. */
export function LanesMap({ level, editable, className }: { level: Level; editable: boolean; className?: string }) {
  const { t } = useTranslation()
  const graph = useLevelGraph(level.id).data
  const lanesView = useLanes().data
  const setLanes = useSetLanes()
  const errorText = useErrorText()
  const canEdit = usePermission('locations.manage') && editable
  const [selected, setSelected] = useState<Set<string>>(new Set())
  const [boxing, setBoxing] = useState(false)
  const [layers, setLayers] = useStored<Layer[]>('eiu-ops-layers', LAYERS, isLayers)

  const offset = lanesView?.offsets[level.id] ?? 0
  const list = useMemo(() => (graph ? corridors(graph, offset) : []), [graph, offset])
  const closed = useMemo(() => new Set(Object.values(lanesView?.fleets ?? {}).flat()), [lanesView])
  const isClosed = (c: Corridor) => c.lanes.some((i) => closed.has(i))
  const closedCount = list.filter(isClosed).length
  const chosen = list.filter((c) => selected.has(c.key))

  const toggle = (key: string) => setSelected((s) => {
    const next = new Set(s)
    if (next.has(key)) next.delete(key)
    else next.add(key)
    return next
  })
  const onBox = (box: L.LatLngBounds) => {
    if (!graph) return
    const min: [number, number] = [box.getWest(), box.getSouth()]
    const max: [number, number] = [box.getEast(), box.getNorth()]
    const point = (i: number): [number, number] => [graph.vertices[i].x, graph.vertices[i].y]
    setSelected((s) => new Set([...s, ...list.filter((c) => segmentInBox(point(c.a), point(c.b), min, max)).map((c) => c.key)]))
  }
  const send = (body: { close?: number[]; open?: number[] }) => setLanes.mutate(body, {
    onSuccess: (r) => {
      toast.success(t('ops.map.lanesSent', { fleets: r.fleets.join(', ') }))
      setSelected(new Set())
    },
    onError: (e) => toast.error(errorText(e)),
  })

  if (!graph) return <Skeleton className={cn('rounded-xl', className)} />

  return (
    <div className="flex flex-col gap-3">
      <div className="flex flex-wrap items-center gap-2">
        <LayerMenu layers={layers} onChange={setLayers} />
        {canEdit && (<>
          <Button size="sm" variant={boxing ? 'primary' : 'secondary'} icon={<BoxIcon className="size-4" />} aria-pressed={boxing} onClick={() => setBoxing((b) => !b)}>
            {t('ops.map.areaSelect')}
          </Button>
          <Button size="sm" variant="danger" icon={<Lock className="size-4" />} disabled={chosen.length === 0} loading={setLanes.isPending}
            onClick={() => send({ close: chosen.flatMap((c) => c.lanes) })}>
            {t('ops.map.closeSelected', { count: chosen.length })}
          </Button>
          <Button size="sm" variant="secondary" icon={<LockOpen className="size-4" />} disabled={chosen.length === 0} loading={setLanes.isPending}
            onClick={() => send({ open: chosen.flatMap((c) => c.lanes) })}>
            {t('ops.map.openSelected', { count: chosen.length })}
          </Button>
          <Button size="sm" variant="ghost" icon={<LockOpen className="size-4" />} disabled={closed.size === 0} onClick={() => send({ open: [...closed] })}>
            {t('ops.map.openAll')}
          </Button>
          {chosen.length > 0 && <Button size="sm" variant="ghost" icon={<X className="size-4" />} onClick={() => setSelected(new Set())}>{t('ops.map.clear')}</Button>}
        </>)}
        <span className="ml-auto text-sm text-slate-500">{t('ops.map.closedCount', { count: closedCount })}</span>
      </div>
      {canEdit && <p className="text-sm text-slate-500">{t('ops.map.lanesHint')}</p>}
      <MapCanvas level={level} className={className} showRobots={layers.includes('robots')}>
        <BoxSelect active={boxing} onBox={onBox} />
        {list.filter((c) => layers.includes('lanes') || isClosed(c) || selected.has(c.key)).map((c) => {
          const va = graph.vertices[c.a]
          const vb = graph.vertices[c.b]
          const style = selected.has(c.key) ? laneStyle.selected : isClosed(c) ? laneStyle.closed : laneStyle.open
          const positions: L.LatLngTuple[] = [[va.y, va.x], [vb.y, vb.x]]
          return (
            <Polyline key={c.key} positions={positions} pathOptions={{ ...style, lineCap: 'round' }} interactive={false} />
          )
        })}
        {canEdit && !boxing && list.map((c) => {
          const va = graph.vertices[c.a]
          const vb = graph.vertices[c.b]
          return (
            <Polyline
              key={`hit-${c.key}`}
              positions={[[va.y, va.x], [vb.y, vb.x]]}
              pathOptions={{ color: '#000', weight: 16, opacity: 0, bubblingMouseEvents: false }}
              eventHandlers={{ click: () => toggle(c.key) }}
            >
              <Tooltip sticky>{`${va.name || `#${c.a}`} ↔ ${vb.name || `#${c.b}`}`}</Tooltip>
            </Polyline>
          )
        })}
        {layers.includes('labels') && graph.vertices.map((v, i) => v.name && (
          <Marker key={i} position={[v.y, v.x]} icon={dotIcon} keyboard={false} interactive={false}>
            <Tooltip permanent direction="top" className="map-label">{v.name}</Tooltip>
          </Marker>
        ))}
      </MapCanvas>
    </div>
  )
}
