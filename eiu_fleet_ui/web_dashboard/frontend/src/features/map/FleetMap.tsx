import 'leaflet/dist/leaflet.css'
import L from 'leaflet'
import { LocateFixed, Minus, Plus } from 'lucide-react'
import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { ImageOverlay, MapContainer, Marker, Polyline, Tooltip, useMap } from 'react-leaflet'
import { useLevelGraph, useLevels, useLocations } from '../../api/queries'
import type { Level } from '../../api/types'
import { Skeleton } from '../../components/ui/States'
import { cn } from '../../lib/cn'
import { useLocalize } from '../../lib/i18nText'
import { useLive } from '../../realtime/store'
import { blueprint, FIT_PADDING_PX, OCCUPANCY } from './blueprint'
import { blueprintChargerIcon, blueprintPin, chargerIcon, dotIcon, dropoffIcon, pickupIcon } from './icons'
import { RobotMarker, type MarkerLook } from './RobotMarker'

const FIT_PADDING: L.PointExpression = [48, 48]
const FIT_MAX_ZOOM = 6
/** Labels of locations closer than this on the same row alternate above and below their marker. */
const LABEL_CLEARANCE_M = 4
const LABEL_ROW_M = 1.5
/** The floor plan stays in the background so places, robot and route stand out. */
const FLOOR_OPACITY = 0.55

/** Map metres to Leaflet coordinates: lat = y, lng = x. */
export function levelBounds(level: Level): L.LatLngBounds {
  const [ox, oy] = level.origin
  return L.latLngBounds([oy, ox], [oy + level.heightPx * level.resolution, ox + level.widthPx * level.resolution])
}

/** Reports the map, follows container resizes, and notes when the user pans or zooms. */
function KeepSized({ onReady, onResize, onUserMove }: {
  onReady: (map: L.Map) => void
  onResize: () => void
  onUserMove: () => void
}) {
  const map = useMap()
  useEffect(() => {
    onReady(map)
    const container = map.getContainer()
    const observer = new ResizeObserver(() => {
      map.invalidateSize({ animate: false })
      onResize()
    })
    observer.observe(container)
    map.on('dragstart', onUserMove)
    container.addEventListener('wheel', onUserMove, { passive: true })
    container.addEventListener('touchstart', onUserMove, { passive: true })
    return () => {
      observer.disconnect()
      map.off('dragstart', onUserMove)
      container.removeEventListener('wheel', onUserMove)
      container.removeEventListener('touchstart', onUserMove)
    }
  }, [map, onReady, onResize, onUserMove])
  return null
}

const controlClass = 'flex size-10 items-center justify-center bg-surface text-slate-700 hover:bg-slate-50 hover:text-slate-900'
const blueprintControlClass = 'flex size-9 items-center justify-center bg-(--bp-chip-bg) text-(--bp-label) hover:bg-(--bp-chip-hover) hover:text-white'
const bpPickup = blueprintPin(blueprint.pickup)
const bpDestination = blueprintPin(blueprint.destination)

/**
 * Recolors the occupancy image (black walls, grey unknown, white free space) into the blueprint palette: walls,
 * edges (geometry), unknown space the map background, free space the inner surface; colors from `blueprint`.
 */
function BlueprintFilter() {
  const channel = (hex: string, i: number) => parseInt(hex.slice(1 + 2 * i, 3 + 2 * i), 16)
  const table = (i: number) => [blueprint.wall, blueprint.wall, blueprint.geometry, blueprint.geometry, blueprint.background, blueprint.surface]
    .map((hex) => (channel(hex, i) / 255).toFixed(3)).join(' ')
  return (
    <svg width="0" height="0" className="absolute" aria-hidden focusable="false">
      <filter id="bp-floor" colorInterpolationFilters="sRGB">
        <feComponentTransfer>
          <feFuncR type="table" tableValues={table(0)} />
          <feFuncG type="table" tableValues={table(1)} />
          <feFuncB type="table" tableValues={table(2)} />
        </feComponentTransfer>
      </filter>
    </svg>
  )
}

const extents = new Map<string, Promise<[number, number, number, number] | null>>()

/** Pixel box [left, top, right, bottom] of the mapped floor (walls and free space) in an occupancy image. */
function floorPixels(url: string): Promise<[number, number, number, number] | null> {
  let found = extents.get(url)
  if (!found) {
    found = new Promise((resolve) => {
      const img = new Image()
      img.onload = () => {
        const canvas = document.createElement('canvas')
        canvas.width = img.naturalWidth
        canvas.height = img.naturalHeight
        const ctx = canvas.getContext('2d', { willReadFrequently: true })
        if (!ctx) return resolve(null)
        ctx.drawImage(img, 0, 0)
        const { data, width, height } = ctx.getImageData(0, 0, canvas.width, canvas.height)
        const cols = new Uint32Array(width)
        const rows = new Uint32Array(height)
        let total = 0
        for (let y = 0; y < height; y++) {
          for (let x = 0; x < width; x++) {
            const v = data[(y * width + x) * 4]
            if (v < OCCUPANCY.wallMax || v > OCCUPANCY.freeMin) {
              cols[x]++
              rows[y]++
              total++
            }
          }
        }
        if (total === 0) return resolve(null)
        // Stray scan noise far from the floor is ignored: each side drops its outermost share of the floor pixels.
        const cut = total * OCCUPANCY.trim
        const edge = (hist: Uint32Array, fromEnd: boolean) => {
          let sum = 0
          for (let i = 0; i < hist.length; i++) {
            const k = fromEnd ? hist.length - 1 - i : i
            sum += hist[k]
            if (sum > cut) return k
          }
          return fromEnd ? hist.length - 1 : 0
        }
        const left = edge(cols, false), right = edge(cols, true), top = edge(rows, false), bottom = edge(rows, true)
        resolve(right < 0 ? null : [left, top, right + 1, bottom + 1])
      }
      img.onerror = () => resolve(null)
      img.src = url
    })
    extents.set(url, found)
  }
  return found
}

/** Map-metre bounds of the mapped floor of a level, once its image is read; null until then or when unreadable. */
function useFloorBounds(level: Level | undefined): L.LatLngBounds | null {
  const [box, setBox] = useState<{ url: string; px: [number, number, number, number] | null } | null>(null)
  useEffect(() => {
    if (!level) return
    let alive = true
    void floorPixels(level.imageUrl).then((px) => alive && setBox({ url: level.imageUrl, px }))
    return () => { alive = false }
  }, [level])
  if (!level || !box || box.url !== level.imageUrl || !box.px) return null
  const [l, t, r, b] = box.px
  const [ox, oy] = level.origin
  const res = level.resolution
  return L.latLngBounds([oy + (level.heightPx - b) * res, ox + l * res], [oy + (level.heightPx - t) * res, ox + r * res])
}

interface FleetMapProps {
  /** Level shown first; the floor switch can change it. */
  levelId?: string
  pickupId?: string
  dropoffId?: string
  robotNames: string[]
  /** Default view is the whole floor, centred; otherwise the delivery's route. */
  fitFloor?: boolean
  /** Size the container to the floor image's aspect ratio. */
  matchAspect?: boolean
  /** A change of this value returns the map to its default view, e.g. on entering full screen. */
  viewKey?: string | number | boolean
  /** Draw the nav graph lanes (an engineering layer; off for users). */
  showGraph?: boolean
  /** Status look and click action of each robot (live map). */
  looks?: Record<string, MarkerLook>
  /** Draw the charging stations of the nav graph. */
  showChargers?: boolean
  /** Dark technical blueprint (live operations): dark floor, styled routes, labels and controls; the view fits the
   * mapped floor (walls and free space of the occupancy image) and the container takes its aspect ratio. */
  variant?: 'default' | 'blueprint'
  /** Robot whose route is drawn as the active one (blueprint). */
  selectedRobot?: string | null
  /** Blueprint: the container keeps the height its classes give it instead of the floor's aspect ratio; the floor is
   * fitted and centred inside. */
  fill?: boolean
  className?: string
}

/** Floor map with campus location labels, the delivery's pins, robots and their route; the nav graph on request. */
export function FleetMap({ levelId, pickupId, dropoffId, robotNames, fitFloor, matchAspect, viewKey, showGraph, looks, showChargers, variant = 'default', selectedRobot, fill, className }: FleetMapProps) {
  const bp = variant === 'blueprint'
  const { t } = useTranslation()
  const localize = useLocalize()
  const levels = useLevels().data
  const [chosenLevel, setChosenLevel] = useState<string>()
  const current = chosenLevel ?? levelId ?? levels?.[0]?.id
  const level = levels?.find((l) => l.id === current)
  const graph = useLevelGraph(current).data
  const locations = useLocations().data
  const robots = useLive((s) => s.robots)
  const [map, setMap] = useState<L.Map | null>(null)
  const userMoved = useRef(false)
  const fitView = useRef<() => void>(() => {})

  const lanes = useMemo(() => {
    if (!graph) return []
    const seen = new Set<string>()
    const out: [L.LatLngTuple, L.LatLngTuple][] = []
    for (const [a, b] of graph.lanes) {
      const key = a < b ? `${a}-${b}` : `${b}-${a}`
      const va = graph.vertices[a]
      const vb = graph.vertices[b]
      if (seen.has(key) || !va || !vb) continue
      seen.add(key)
      out.push([[va.y, va.x], [vb.y, vb.x]])
    }
    return out
  }, [graph])

  const onLevel = useMemo(() => (locations ?? []).filter((l) => l.levelId === current), [locations, current])
  const labelBelow = useMemo(() => new Set(onLevel
    .filter((l) => onLevel.some((o) => o !== l && o.x < l.x && l.x - o.x < LABEL_CLEARANCE_M && Math.abs(o.y - l.y) < LABEL_ROW_M))
    .map((l) => l.id)), [onLevel])
  const pickup = onLevel.find((l) => l.id === pickupId)
  const dropoff = onLevel.find((l) => l.id === dropoffId)
  const routes = robotNames
    .map((name) => robots[name])
    .filter((r) => r && r.levelId === current && r.path.length > 1)
    .map((r) => ({ name: r!.name, points: r!.path.map(([x, y]) => [y, x] as L.LatLngTuple) }))

  const fitRoute = () => {
    if (!map || !level) return
    const points: L.LatLngTuple[] = []
    if (pickup) points.push([pickup.y, pickup.x])
    if (dropoff) points.push([dropoff.y, dropoff.x])
    robotNames.forEach((n) => {
      const r = robots[n]
      if (r && r.levelId === current) points.push([r.y, r.x])
    })
    map.fitBounds(points.length > 1 ? L.latLngBounds(points) : levelBounds(level), { padding: FIT_PADDING, maxZoom: FIT_MAX_ZOOM })
  }

  const floorBounds = useFloorBounds(bp ? level : undefined)
  const content = bp ? floorBounds : null
  // Blueprint: the frame is as tall as the floor needs at the frame's width, padding included on every side.
  const frame = useRef<HTMLDivElement>(null)
  const [frameWidth, setFrameWidth] = useState(0)
  useEffect(() => {
    const el = frame.current
    if (!el || !bp) return
    const observer = new ResizeObserver(([entry]) => setFrameWidth(entry.contentRect.width))
    observer.observe(el)
    return () => observer.disconnect()
  }, [bp, level])
  const fitFloorNow = () => {
    if (!map || !level) return
    if (bp && content) map.fitBounds(content, { animate: false, padding: [FIT_PADDING_PX, FIT_PADDING_PX] })
    else map.fitBounds(levelBounds(level), { animate: false })
  }

  /** Default view: the whole floor, or the delivery's route; kept on resize until the user moves the map. */
  fitView.current = () => {
    if (!map || !level || userMoved.current) return
    if (fitFloor) fitFloorNow()
    else fitRoute()
  }
  const ready = !!map && !!level && (!pickupId || !!pickup) && (!dropoffId || !!dropoff)
  useEffect(() => {
    userMoved.current = false
  }, [viewKey])
  useEffect(() => {
    if (ready) fitView.current()
  }, [ready, current, viewKey, content])
  const onResize = useCallback(() => fitView.current(), [])
  const onUserMove = useCallback(() => { userMoved.current = true }, [])
  const onReady = useCallback((m: L.Map) => {
    userMoved.current = false
    setMap(m)
  }, [])
  const recenter = () => {
    userMoved.current = true
    if (fitFloor) fitFloorNow()
    else fitRoute()
  }

  if (!level) return <Skeleton className={cn('rounded-xl', className)} />
  const bounds = levelBounds(level)
  const floorAspect = content ? (content.getEast() - content.getWest()) / (content.getNorth() - content.getSouth()) : null
  const frameHeight = bp && !fill && floorAspect && frameWidth > 0
    ? Math.round((frameWidth - 2 * FIT_PADDING_PX) / floorAspect + 2 * FIT_PADDING_PX)
    : undefined
  const aspect = !bp && matchAspect ? `${level.widthPx} / ${level.heightPx}` : undefined
  const control = bp ? blueprintControlClass : controlClass

  return (
    <div
      ref={frame}
      className={cn('relative isolate overflow-hidden rounded-xl', bp ? 'map-blueprint bg-(--bp-bg) ring-1 ring-(--bp-line)' : 'bg-slate-100 ring-1 ring-slate-200', className)}
      style={frameHeight ? { height: frameHeight } : aspect ? { aspectRatio: aspect } : undefined}
    >
      {bp && <BlueprintFilter />}
      <MapContainer
        key={level.id}
        crs={L.CRS.Simple}
        bounds={bounds}
        boundsOptions={{ padding: [12, 12] }}
        maxBounds={bounds.pad(0.5)}
        maxBoundsViscosity={0.8}
        minZoom={1}
        maxZoom={8}
        zoomSnap={0.05}
        zoomDelta={0.5}
        zoomControl={false}
        attributionControl={false}
        className="h-full w-full"
      >
        <KeepSized onReady={onReady} onResize={onResize} onUserMove={onUserMove} />
        <ImageOverlay url={level.imageUrl} bounds={bounds} opacity={bp ? 1 : FLOOR_OPACITY} />
        {showGraph && lanes.map((lane, i) => (
          <Polyline key={i} positions={lane} pathOptions={{ color: '#94a3b8', weight: 2, opacity: 0.55, interactive: false }} />
        ))}
        {!bp && routes.map((r) => (
          <Polyline key={`${r.name}-casing`} positions={r.points} pathOptions={{ color: '#ffffff', weight: 9, opacity: 0.9, interactive: false }} />
        ))}
        {!bp && routes.map((r) => (
          <Polyline key={r.name} positions={r.points} pathOptions={{ color: '#1a5cf5', weight: 5, lineCap: 'round', lineJoin: 'round', interactive: false }} />
        ))}
        {bp && routes.filter((r) => r.name !== selectedRobot).map((r) => (
          <Polyline key={`${r.name}-rest`} positions={r.points}
            pathOptions={{ color: blueprint.routeRemaining, weight: 3, dashArray: '6 7', lineCap: 'round', lineJoin: 'round' }}
            eventHandlers={{ mouseover: (e) => e.target.setStyle({ color: blueprint.routeHover }), mouseout: (e) => e.target.setStyle({ color: blueprint.routeRemaining }) }} />
        ))}
        {bp && routes.filter((r) => r.name === selectedRobot).map((r) => [
          <Polyline key={`${r.name}-glow`} positions={r.points} pathOptions={{ color: blueprint.routeActive, weight: 10, opacity: 0.18, interactive: false }} />,
          <Polyline key={`${r.name}-casing`} positions={r.points} pathOptions={{ color: blueprint.routeCasing, weight: 7, opacity: 0.9, interactive: false }} />,
          <Polyline key={`${r.name}-active`} positions={r.points} pathOptions={{ color: blueprint.routeActive, weight: 4, lineCap: 'round', lineJoin: 'round', interactive: false }} />,
        ])}
        {onLevel.filter((l) => l !== pickup && l !== dropoff).map((l) => (
          <Marker key={l.id} position={[l.y, l.x]} icon={dotIcon} keyboard={false}>
            <Tooltip permanent direction={labelBelow.has(l.id) ? 'bottom' : 'top'} className={bp ? 'map-label map-label--poi' : 'map-label'}>{localize(l.name)}</Tooltip>
          </Marker>
        ))}
        {showChargers && graph?.vertices.filter((v) => v.charger).map((v) => (
          <Marker key={`charger-${v.name}`} position={[v.y, v.x]} icon={bp ? blueprintChargerIcon : chargerIcon} keyboard={false} title={v.name} />
        ))}
        {pickup && (
          <Marker position={[pickup.y, pickup.x]} icon={bp ? bpPickup : pickupIcon} zIndexOffset={500}>
            <Tooltip permanent direction="top" className="map-label map-label--pickup">{localize(pickup.name)}</Tooltip>
          </Marker>
        )}
        {dropoff && (
          <Marker position={[dropoff.y, dropoff.x]} icon={bp ? bpDestination : dropoffIcon} zIndexOffset={500}>
            <Tooltip permanent direction="top" className="map-label map-label--dropoff">{localize(dropoff.name)}</Tooltip>
          </Marker>
        )}
        {robotNames.map((name) => <RobotMarker key={name} name={name} levelId={level.id} look={looks?.[name]} />)}
      </MapContainer>

      <div className={cn('absolute bottom-3 left-3 z-[1000] flex flex-col overflow-hidden rounded-xl', bp ? 'divide-y divide-(--bp-line) ring-1 ring-(--bp-line)' : 'divide-y divide-slate-200 shadow-lg ring-1 ring-slate-900/10')}>
        <button type="button" aria-label={t('tracking.zoomIn')} title={t('tracking.zoomIn')} className={control} onClick={() => { onUserMove(); map?.zoomIn() }}><Plus className="size-5" /></button>
        <button type="button" aria-label={t('tracking.zoomOut')} title={t('tracking.zoomOut')} className={control} onClick={() => { onUserMove(); map?.zoomOut() }}><Minus className="size-5" /></button>
        <button type="button" aria-label={t('tracking.recenter')} title={t('tracking.recenter')} className={control} onClick={recenter}><LocateFixed className="size-5" /></button>
      </div>

      {levels && levels.length > 0 && (
        <div role="radiogroup" aria-label={t('tracking.floors')} className="absolute top-3 right-3 z-[1000] flex flex-col gap-2">
          {levels.map((l) => (
            <button
              key={l.id}
              type="button"
              role="radio"
              aria-checked={l.id === current}
              onClick={() => setChosenLevel(l.id)}
              className={cn(bp
                ? cn('h-8 min-w-10 rounded-lg px-2.5 text-xs font-semibold ring-1 ring-(--bp-line)', l.id === current ? 'bg-[#3b82f6] text-white' : 'bg-(--bp-chip-bg) text-(--bp-label) hover:bg-(--bp-chip-hover)')
                : cn('h-10 min-w-12 rounded-xl px-3 text-sm font-bold shadow-lg ring-1 ring-slate-900/10', l.id === current ? 'bg-brand-600 text-white' : 'bg-surface text-slate-700 hover:bg-slate-50'),
              )}
            >
              {localize(l.label)}
            </button>
          ))}
        </div>
      )}
    </div>
  )
}

export function MapLegend({ className }: { className?: string }) {
  const { t } = useTranslation()
  const item = 'flex h-9 items-center gap-2 rounded-xl bg-surface px-3 text-sm font-medium text-slate-700 ring-1 ring-slate-200'
  return (
    <div className={cn('flex flex-wrap gap-2', className)}>
      <span className={item}><span className="size-3.5 rounded-full border-[3px] border-brand-600" />{t('tracking.legend.pickup')}</span>
      <span className={item}><span className="h-1 w-5 rounded-full bg-brand-600" />{t('tracking.legend.route')}</span>
      <span className={item}><span className="size-3.5 rounded-full bg-emerald-500" />{t('tracking.legend.destination')}</span>
    </div>
  )
}
