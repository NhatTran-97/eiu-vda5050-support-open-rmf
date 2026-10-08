// Map levels and nav graphs exported from eiu_fleet_ui/maps by scripts/export_site.py.
import type { LevelGraph } from '../api/types'
import siteJson from './data/site.json'
import { length, routeTo, type Pt } from './geometry'
import { makePlan, type Plan } from './plan'
import { demoSettings } from './settings'

export interface SiteLevel {
  id: string
  image: string
  origin: [number, number]
  resolution: number
  widthPx: number
  heightPx: number
  graph: { vertices: LevelGraph['vertices']; lanes: [number, number][] }
}

export const siteLevels: SiteLevel[] = (siteJson.levels as SiteLevel[]).map((l) => ({
  ...l,
  origin: [l.origin[0], l.origin[1]],
  graph: { vertices: l.graph.vertices, lanes: l.graph.lanes.map(([a, b]) => [a, b] as [number, number]) },
}))

export function levelGraph(levelId: string): LevelGraph | null {
  const level = siteLevels.find((l) => l.id === levelId)
  return level ? { levelId, vertices: level.graph.vertices, lanes: level.graph.lanes } : null
}

export function vertexIndex(levelId: string, waypoint: string): number {
  return levelGraph(levelId)?.vertices.findIndex((v) => v.name === waypoint) ?? -1
}

export function waypointPoint(levelId: string, waypoint: string): Pt | null {
  const graph = levelGraph(levelId)
  const v = graph?.vertices.find((w) => w.name === waypoint)
  return v ? { x: v.x, y: v.y } : null
}

/** Plan of a patrol: to the first stop, then through the stops `rounds` times without waiting. */
export function planPatrol(levelId: string, start: Pt, stopWps: string[], rounds: number,
                           startAt: number, battery: number): Plan | null {
  const graph = levelGraph(levelId)
  const stops = stopWps.map((w) => vertexIndex(levelId, w))
  if (!graph || stops.some((i) => i < 0)) return null
  const toFirst = routeTo(graph, start, stops[0])
  if (!toFirst) return null
  const loop: Pt[] = [{ x: graph.vertices[stops[0]].x, y: graph.vertices[stops[0]].y }]
  const sequence = [...stops.slice(1), ...Array.from({ length: rounds - 1 }, () => stops).flat()]
  for (const target of sequence) {
    const leg = routeTo(graph, loop[loop.length - 1], target)
    if (!leg) return null
    loop.push(...leg.slice(1))
  }
  const timing = { speedMps: demoSettings.robotSpeedMps, loadingDwellS: 0, collectDwellS: 0, arrivingSoonS: 0 }
  return makePlan(toFirst, length(loop) > 0 ? loop : [loop[0], loop[0]], startAt, timing, battery)
}

/** Plan from a robot pose through the pickup waypoint to the drop-off waypoint, or null when unreachable. */
export function planDelivery(levelId: string, start: Pt, pickupWp: string, dropoffWp: string,
                             startAt: number, battery: number): Plan | null {
  const graph = levelGraph(levelId)
  const pickup = vertexIndex(levelId, pickupWp)
  const dropoff = vertexIndex(levelId, dropoffWp)
  if (!graph || pickup < 0 || dropoff < 0) return null
  const toPickup = routeTo(graph, start, pickup)
  const toDropoff = routeTo(graph, graph.vertices[pickup], dropoff)
  if (!toPickup || !toDropoff) return null
  return makePlan(toPickup, toDropoff, startAt, {
    speedMps: demoSettings.robotSpeedMps,
    loadingDwellS: demoSettings.loadingDwellS,
    collectDwellS: demoSettings.collectDwellS,
    arrivingSoonS: demoSettings.arrivingSoonS,
  }, battery)
}
