import L from 'leaflet'
import { Minus, Plus, Scan } from 'lucide-react'
import { useEffect, useMemo, useRef, useState, type ReactNode } from 'react'
import { useTranslation } from 'react-i18next'
import { ImageOverlay, MapContainer, useMap } from 'react-leaflet'
import type { Level } from '../../api/types'
import { cn } from '../../lib/cn'
import { useLive } from '../../realtime/store'
import { levelBounds } from '../map/FleetMap'
import { RobotMarker } from '../map/RobotMarker'

/** Keeps the whole floor in view until the user zooms; follows container resizes. */
function FitFloor({ bounds, onMap }: { bounds: L.LatLngBounds; onMap: (map: L.Map) => void }) {
  const map = useMap()
  useEffect(() => {
    onMap(map)
    let userZoomed = false
    const fit = () => {
      map.invalidateSize({ animate: false })
      if (!userZoomed) map.fitBounds(bounds, { animate: false })
    }
    const zoomed = () => { userZoomed = true }
    const observer = new ResizeObserver(fit)
    observer.observe(map.getContainer())
    map.getContainer().addEventListener('wheel', zoomed, { passive: true })
    fit()
    return () => {
      observer.disconnect()
      map.getContainer().removeEventListener('wheel', zoomed)
    }
  }, [map, bounds, onMap])
  return null
}

/** While `active`, dragging draws a rectangle instead of panning (native image drag is blocked); `onBox` gets its bounds. */
export function BoxSelect({ active, onBox }: { active: boolean; onBox: (bounds: L.LatLngBounds) => void }) {
  const map = useMap()
  const onBoxRef = useRef(onBox)
  onBoxRef.current = onBox
  useEffect(() => {
    if (!active) return
    const container = map.getContainer()
    map.dragging.disable()
    container.style.touchAction = 'none'
    container.style.cursor = 'crosshair'
    let start: L.LatLng | null = null
    let rect: L.Rectangle | null = null
    const down = (e: PointerEvent) => {
      if (e.button !== 0 || (e.target as HTMLElement).closest('button')) return
      e.preventDefault()
      start = map.mouseEventToLatLng(e)
      container.setPointerCapture(e.pointerId)
    }
    const move = (e: PointerEvent) => {
      if (!start) return
      const box = L.latLngBounds(start, map.mouseEventToLatLng(e))
      if (rect) rect.setBounds(box)
      else rect = L.rectangle(box, { color: '#f59e0b', weight: 2, dashArray: '6 4', fillOpacity: 0.12, interactive: false }).addTo(map)
    }
    const end = (e: PointerEvent, apply: boolean) => {
      if (apply && start && rect) onBoxRef.current(L.latLngBounds(start, map.mouseEventToLatLng(e)))
      rect?.remove()
      rect = null
      start = null
    }
    const up = (e: PointerEvent) => end(e, true)
    const cancel = (e: PointerEvent) => end(e, false)
    const noDrag = (e: Event) => e.preventDefault()
    container.addEventListener('pointerdown', down)
    container.addEventListener('pointermove', move)
    container.addEventListener('pointerup', up)
    container.addEventListener('pointercancel', cancel)
    container.addEventListener('dragstart', noDrag)
    return () => {
      container.removeEventListener('pointerdown', down)
      container.removeEventListener('pointermove', move)
      container.removeEventListener('pointerup', up)
      container.removeEventListener('pointercancel', cancel)
      container.removeEventListener('dragstart', noDrag)
      rect?.remove()
      container.style.touchAction = ''
      container.style.cursor = ''
      map.dragging.enable()
    }
  }, [map, active])
  return null
}

const controlClass = 'flex size-10 items-center justify-center bg-surface text-slate-700 hover:bg-slate-50 hover:text-slate-900'

/** Floor image in map metres, sized to the image's aspect ratio, with zoom controls, live robots and the layers given as children. */
export function MapCanvas({ level, children, showRobots = true, className }: {
  level: Level
  children?: ReactNode
  showRobots?: boolean
  className?: string
}) {
  const { t } = useTranslation()
  const [map, setMap] = useState<L.Map | null>(null)
  const robotNames = useLive((s) => Object.keys(s.robots).sort().join('\n'))
  const bounds = useMemo(() => levelBounds(level), [level])

  return (
    <div
      className={cn('relative isolate min-h-72 w-full overflow-hidden rounded-xl bg-slate-100 ring-1 ring-slate-200', className)}
      style={{ aspectRatio: `${level.widthPx} / ${level.heightPx}` }}
    >
      <MapContainer
        key={level.id}
        crs={L.CRS.Simple}
        bounds={bounds}
        maxBounds={bounds.pad(0.5)}
        maxBoundsViscosity={0.8}
        minZoom={1}
        maxZoom={8}
        zoomSnap={0.05}
        zoomDelta={0.5}
        zoomControl={false}
        attributionControl={false}
        doubleClickZoom={false}
        className="h-full w-full"
      >
        <FitFloor bounds={bounds} onMap={setMap} />
        <ImageOverlay url={level.imageUrl} bounds={bounds} />
        {children}
        {showRobots && robotNames && robotNames.split('\n').map((name) => <RobotMarker key={name} name={name} levelId={level.id} />)}
      </MapContainer>
      <div className="absolute bottom-3 left-3 z-[1000] flex flex-col divide-y divide-slate-200 overflow-hidden rounded-xl shadow-lg ring-1 ring-slate-900/10">
        <button type="button" aria-label={t('tracking.zoomIn')} title={t('tracking.zoomIn')} className={controlClass} onClick={() => map?.zoomIn()}><Plus className="size-5" /></button>
        <button type="button" aria-label={t('tracking.zoomOut')} title={t('tracking.zoomOut')} className={controlClass} onClick={() => map?.zoomOut()}><Minus className="size-5" /></button>
        <button type="button" aria-label={t('tracking.recenter')} title={t('tracking.recenter')} className={controlClass} onClick={() => map?.fitBounds(bounds)}><Scan className="size-5" /></button>
      </div>
    </div>
  )
}
