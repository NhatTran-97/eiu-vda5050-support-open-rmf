import L from 'leaflet'
import { useEffect, useRef } from 'react'
import { useMap } from 'react-leaflet'
import { useLive, useRobot } from '../../realtime/store'
import { robotIcon } from './icons'

/** How a robot is drawn on the live map; without it the plain robot tile is used. */
export interface MarkerLook {
  icon: L.DivIcon
  onSelect?: () => void
  /** Class of the robot's name label. */
  labelClass?: string
  /** Hover tooltip, e.g. "DEL-01 · Navigating". */
  title?: string
}

/** A robot marker that glides between stream updates instead of jumping. */
export function RobotMarker({ name, levelId, look }: { name: string; levelId: string; look?: MarkerLook }) {
  const map = useMap()
  const robot = useRobot(name)
  const marker = useRef<L.Marker | null>(null)
  const frame = useRef<number | null>(null)
  const select = useRef(look?.onSelect)
  select.current = look?.onSelect
  const visible = !!robot && robot.levelId === levelId
  const x = robot?.x
  const y = robot?.y

  useEffect(() => {
    const initial = useLive.getState().robots[name]
    if (!visible || !initial) return
    const m = L.marker([initial.y, initial.x], { icon: robotIcon, keyboard: true, zIndexOffset: 1000, title: name })
      .bindTooltip(name, { permanent: true, direction: 'right', offset: [20, 0], className: 'map-label' })
      .on('click', () => select.current?.())
      .addTo(map)
    marker.current = m
    return () => {
      if (frame.current !== null) cancelAnimationFrame(frame.current)
      m.remove()
      marker.current = null
    }
  }, [map, name, visible])

  useEffect(() => {
    const m = marker.current
    if (!m || !look) return
    m.setIcon(look.icon)
    m.unbindTooltip().bindTooltip(name, { permanent: true, direction: 'right', offset: [22, 0], className: look.labelClass ?? 'map-label' })
    const el = m.getElement()
    if (el && look.title) el.title = look.title
  }, [look?.icon, look?.labelClass, look?.title, visible, name]) // eslint-disable-line react-hooks/exhaustive-deps

  useEffect(() => {
    const m = marker.current
    if (!m || x === undefined || y === undefined) return
    const from = m.getLatLng()
    const duration = useLive.getState().periodMs
    const start = performance.now()
    if (frame.current !== null) cancelAnimationFrame(frame.current)
    const step = (now: number) => {
      const k = Math.min(1, (now - start) / duration)
      m.setLatLng([from.lat + (y - from.lat) * k, from.lng + (x - from.lng) * k])
      frame.current = k < 1 ? requestAnimationFrame(step) : null
    }
    frame.current = requestAnimationFrame(step)
  }, [x, y])

  return null
}
