import { describe, expect, it } from 'vitest'
import { addLane, corridors, removeVertex, segmentInBox } from './graph'

const v = (name: string) => ({ name, x: 0, y: 0, charger: false, attrs: {} })

describe('corridors', () => {
  it('pairs both directions and numbers lanes from the level offset', () => {
    const graph = { levelId: 'L1', vertices: [v('a'), v('b'), v('c')].map(({ attrs: _, ...rest }) => rest), lanes: [[0, 1], [1, 0], [1, 2]] as [number, number][] }
    expect(corridors(graph, 10)).toEqual([
      { key: '0-1', a: 0, b: 1, lanes: [10, 11] },
      { key: '1-2', a: 1, b: 2, lanes: [12] },
    ])
  })
})

describe('draft edits', () => {
  const draft = { vertices: [v('a'), v('b'), v('c')], lanes: [{ from: 0, to: 1, attrs: {} }, { from: 1, to: 2, attrs: {} }, { from: 2, to: 0, attrs: {} }] }

  it('removes a vertex with its lanes and renumbers the rest', () => {
    const next = removeVertex(draft, 1)
    expect(next.vertices.map((x) => x.name)).toEqual(['a', 'c'])
    expect(next.lanes).toEqual([{ from: 1, to: 0, attrs: {} }])
  })

  it('adds missing directions only', () => {
    expect(addLane(draft, 0, 1, true).lanes.slice(3)).toEqual([{ from: 1, to: 0, attrs: {} }])
    expect(addLane(draft, 0, 1, false).lanes).toHaveLength(3)
  })
})

describe('segmentInBox', () => {
  const min: [number, number] = [0, 0]
  const max: [number, number] = [2, 2]
  it('accepts a segment that only crosses the box', () => {
    expect(segmentInBox([-5, 1], [5, 1], min, max)).toBe(true)
    expect(segmentInBox([1, -5], [1, 5], min, max)).toBe(true)
  })
  it('accepts a segment inside and refuses one outside', () => {
    expect(segmentInBox([0.5, 0.5], [1, 1], min, max)).toBe(true)
    expect(segmentInBox([-5, 3], [5, 3], min, max)).toBe(false)
    expect(segmentInBox([3, -1], [5, 5], min, max)).toBe(false)
  })
})
