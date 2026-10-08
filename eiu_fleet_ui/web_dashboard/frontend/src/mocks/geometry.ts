// Polylines and shortest paths on the nav graph, in map metres.
import type { LevelGraph } from '../api/types'

export interface Pt {
  x: number
  y: number
}

export function dist(a: Pt, b: Pt): number {
  return Math.hypot(b.x - a.x, b.y - a.y)
}

export function length(points: Pt[]): number {
  let total = 0
  for (let i = 1; i < points.length; i++) total += dist(points[i - 1], points[i])
  return total
}

/** Point and heading at distance d along the polyline, clamped to its ends. */
export function pointAt(points: Pt[], d: number): Pt & { yaw: number } {
  if (points.length === 0) return { x: 0, y: 0, yaw: 0 }
  if (points.length === 1) return { ...points[0], yaw: 0 }
  let left = Math.max(0, d)
  for (let i = 1; i < points.length; i++) {
    const a = points[i - 1]
    const b = points[i]
    const seg = dist(a, b)
    const yaw = Math.atan2(b.y - a.y, b.x - a.x)
    if (left <= seg || i === points.length - 1) {
      const t = seg === 0 ? 1 : Math.min(1, left / seg)
      return { x: a.x + (b.x - a.x) * t, y: a.y + (b.y - a.y) * t, yaw }
    }
    left -= seg
  }
  const last = points[points.length - 1]
  return { ...last, yaw: 0 }
}

/** The part of the polyline after distance d. */
export function remainder(points: Pt[], d: number): Pt[] {
  if (points.length < 2) return [...points]
  const out: Pt[] = [pointAt(points, d)]
  let walked = 0
  for (let i = 1; i < points.length; i++) {
    walked += dist(points[i - 1], points[i])
    if (walked > d) out.push(points[i])
  }
  return out
}

export function nearestVertex(graph: LevelGraph, p: Pt): number {
  let best = 0
  let bestD = Infinity
  graph.vertices.forEach((v, i) => {
    const d = dist(v, p)
    if (d < bestD) {
      bestD = d
      best = i
    }
  })
  return best
}

/** Vertex indices of the shortest path over directed lanes, or null when unreachable. */
export function shortestPath(graph: LevelGraph, from: number, to: number): number[] | null {
  const n = graph.vertices.length
  const adj: number[][] = Array.from({ length: n }, () => [])
  for (const [a, b] of graph.lanes) adj[a]?.push(b)
  const cost = new Array<number>(n).fill(Infinity)
  const prev = new Array<number>(n).fill(-1)
  const done = new Array<boolean>(n).fill(false)
  cost[from] = 0
  for (let k = 0; k < n; k++) {
    let u = -1
    for (let i = 0; i < n; i++) if (!done[i] && (u === -1 || cost[i] < cost[u])) u = i
    if (u === -1 || cost[u] === Infinity) break
    if (u === to) break
    done[u] = true
    for (const v of adj[u]) {
      const c = cost[u] + dist(graph.vertices[u], graph.vertices[v])
      if (c < cost[v]) {
        cost[v] = c
        prev[v] = u
      }
    }
  }
  if (cost[to] === Infinity) return null
  const path = [to]
  while (path[0] !== from) path.unshift(prev[path[0]])
  return path
}

/** Route from an arbitrary point to a vertex: straight to the nearest vertex, then along lanes. */
export function routeTo(graph: LevelGraph, start: Pt, target: number): Pt[] | null {
  const entry = nearestVertex(graph, start)
  const path = shortestPath(graph, entry, target)
  if (!path) return null
  const points: Pt[] = [{ x: start.x, y: start.y }]
  for (const i of path) {
    const v = graph.vertices[i]
    if (dist(points[points.length - 1], v) > 1e-6) points.push({ x: v.x, y: v.y })
  }
  return points
}
