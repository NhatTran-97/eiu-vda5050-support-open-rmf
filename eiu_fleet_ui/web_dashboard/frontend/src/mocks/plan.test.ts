import { describe, expect, it } from 'vitest'
import { makePlan, milestones, poseAt, stageAt } from './plan'

const timing = { speedMps: 1, loadingDwellS: 10, collectDwellS: 5, arrivingSoonS: 3 }
const toPickup = [{ x: 0, y: 0 }, { x: 4, y: 0 }]
const toDropoff = [{ x: 4, y: 0 }, { x: 4, y: 6 }]
const plan = makePlan(toPickup, toDropoff, 1000, timing, 80)

describe('delivery plan', () => {
  it('schedules stages from distance, speed and dwell times', () => {
    expect(plan.pickupArriveAt).toBe(1000 + 4000)
    expect(plan.departAt).toBe(5000 + 10_000)
    expect(plan.arriveAt).toBe(15_000 + 6000)
    expect(plan.arrivingAt).toBe(21_000 - 3000)
    expect(plan.completeAt).toBe(21_000 + 5000)
    expect(milestones(plan).map(([, at]) => at)).toEqual([5000, 15_000, 18_000, 21_000, 26_000])
  })

  it('reports the stage at any time', () => {
    expect(stageAt(plan, 1000)).toBe('to_pickup')
    expect(stageAt(plan, 6000)).toBe('at_pickup')
    expect(stageAt(plan, 16_000)).toBe('in_transit')
    expect(stageAt(plan, 22_000)).toBe('arrived')
    expect(stageAt(plan, 30_000)).toBe('completed')
  })

  it('places the robot along the route', () => {
    expect(poseAt(plan, 3000)).toMatchObject({ x: 2, y: 0, moving: true, remainingM: 8 })
    expect(poseAt(plan, 10_000)).toMatchObject({ x: 4, y: 0, moving: false, remainingM: 6 })
    expect(poseAt(plan, 18_000)).toMatchObject({ x: 4, y: 3, moving: true, remainingM: 3 })
    expect(poseAt(plan, 40_000)).toMatchObject({ x: 4, y: 6, moving: false, remainingM: 0, path: [] })
  })
})
