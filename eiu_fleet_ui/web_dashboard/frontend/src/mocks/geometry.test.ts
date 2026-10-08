import { describe, expect, it } from 'vitest'
import { length, pointAt, remainder, routeTo, shortestPath } from './geometry'
import { levelGraph, siteLevels, vertexIndex } from './site'

const levelId = siteLevels[0].id
const graph = levelGraph(levelId)!

describe('nav graph routing', () => {
  it('connects every waypoint to every other waypoint', () => {
    const n = graph.vertices.length
    for (let a = 0; a < n; a++) {
      for (let b = 0; b < n; b++) expect(shortestPath(graph, a, b), `${a} -> ${b}`).not.toBeNull()
    }
  })

  it('starts a route at an off-graph point and ends on the target waypoint', () => {
    const target = vertexIndex(levelId, graph.vertices[graph.vertices.length - 1].name)
    const start = { x: graph.vertices[0].x + 0.3, y: graph.vertices[0].y + 0.2 }
    const route = routeTo(graph, start, target)!
    expect(route[0]).toEqual(start)
    expect(route[route.length - 1]).toEqual({ x: graph.vertices[target].x, y: graph.vertices[target].y })
  })
})

describe('polylines', () => {
  const line = [{ x: 0, y: 0 }, { x: 3, y: 0 }, { x: 3, y: 4 }]

  it('measures length', () => expect(length(line)).toBe(7))

  it('walks along segments and clamps at the ends', () => {
    expect(pointAt(line, 1.5)).toMatchObject({ x: 1.5, y: 0 })
    expect(pointAt(line, 5)).toMatchObject({ x: 3, y: 2 })
    expect(pointAt(line, 99)).toMatchObject({ x: 3, y: 4 })
    expect(pointAt(line, -1)).toMatchObject({ x: 0, y: 0 })
  })

  it('keeps the part after a distance', () => {
    expect(remainder(line, 5)).toEqual([{ x: 3, y: 2, yaw: Math.PI / 2 }, { x: 3, y: 4 }])
  })
})
