// Nav graph helpers of the operations map; plain functions, tested without a browser.
import type { GraphLaneEdit, GraphVertexEdit, LevelGraph } from '../../api/types'

/** Both directions of a corridor share one line on the map; RMF closes each direction by its index. */
export interface Corridor {
  key: string
  a: number
  b: number
  /** Lane indices in RMF numbering. */
  lanes: number[]
}

export function corridors(graph: LevelGraph, offset: number): Corridor[] {
  const byKey = new Map<string, Corridor>()
  graph.lanes.forEach(([a, b], i) => {
    const key = a < b ? `${a}-${b}` : `${b}-${a}`
    const corridor = byKey.get(key)
    if (corridor) corridor.lanes.push(offset + i)
    else byKey.set(key, { key, a: Math.min(a, b), b: Math.max(a, b), lanes: [offset + i] })
  })
  return [...byKey.values()]
}

/** Whether segment a-b crosses or touches the box (Liang–Barsky clipping). */
export function segmentInBox(a: [number, number], b: [number, number], min: [number, number], max: [number, number]): boolean {
  const d = [b[0] - a[0], b[1] - a[1]]
  let t0 = 0
  let t1 = 1
  for (let axis = 0; axis < 2; axis++) {
    for (const [p, q] of [[-d[axis], a[axis] - min[axis]], [d[axis], max[axis] - a[axis]]]) {
      if (p === 0) {
        if (q < 0) return false
        continue
      }
      const t = q / p
      if (p < 0) t0 = Math.max(t0, t)
      else t1 = Math.min(t1, t)
      if (t0 > t1) return false
    }
  }
  return true
}

export interface Draft {
  vertices: GraphVertexEdit[]
  lanes: GraphLaneEdit[]
}

export const pairKey = (a: number, b: number) => (a < b ? `${a}-${b}` : `${b}-${a}`)

/** Removes vertex `index` with its lanes and renumbers the lanes after it. */
export function removeVertex(draft: Draft, index: number): Draft {
  const shift = (i: number) => (i > index ? i - 1 : i)
  return {
    vertices: draft.vertices.filter((_, i) => i !== index),
    lanes: draft.lanes.filter((l) => l.from !== index && l.to !== index).map((l) => ({ ...l, from: shift(l.from), to: shift(l.to) })),
  }
}

/** Adds the lane a→b (and b→a when `both`) unless it exists. */
export function addLane(draft: Draft, a: number, b: number, both: boolean): Draft {
  const has = (f: number, t: number) => draft.lanes.some((l) => l.from === f && l.to === t)
  const lanes = [...draft.lanes]
  if (!has(a, b)) lanes.push({ from: a, to: b, attrs: {} })
  if (both && !has(b, a)) lanes.push({ from: b, to: a, attrs: {} })
  return { ...draft, lanes }
}
