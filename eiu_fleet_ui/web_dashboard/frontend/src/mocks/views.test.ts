import { describe, expect, it } from 'vitest'
import type { DeliveryRow } from './schema'
import { seed, taskRow } from './seed'
import { toDelivery } from './views'

const data = seed(Date.UTC(2026, 9, 2, 3, 0))
const student = data.users.find((u) => u.role === 'operator')!
const admin = data.users.find((u) => u.role === 'admin')!

function row(patch: Partial<DeliveryRow>): DeliveryRow {
  return taskRow({
    id: 1, kind: 'delivery', requesterId: student.id, pickupId: 'library', dropoffId: 'room_204', createdAt: 100, status: 'queued',
    params: { pickup: 'library', dropoff: 'room_204' }, zones: ['building_a', 'building_b'], ...patch,
  })
}

describe('delivery view', () => {
  it('marks the first unfinished step current while the delivery runs', () => {
    const d = toDelivery(data, row({
      status: 'in_transit', robotName: 'D01',
      events: [{ type: 'requested', at: 100 }, { type: 'assigned', at: 110 }, { type: 'picked_up', at: 200 }],
    }), student)
    expect(d.timeline.map((s) => s.state)).toEqual(['done', 'done', 'current', 'pending', 'pending'])
    expect(d.timeline[2].at).toBe(200)
    expect(d.group).toBe('active')
  })

  it('skips the remaining steps of a cancelled delivery', () => {
    const d = toDelivery(data, row({ status: 'cancelled', finishedAt: 300 }), student)
    expect(d.timeline.map((s) => s.state)).toEqual(['done', 'skipped', 'skipped', 'skipped', 'skipped'])
    expect(d.actions).toEqual({ cancel: false, pause: false, resume: false, reassign: false, track: false, reorder: true })
  })

  it('lets an operator with task.cancel cancel before pickup and an admin cancel any delivery', () => {
    expect(toDelivery(data, row({ status: 'at_pickup' }), student).actions.cancel).toBe(true)
    expect(toDelivery(data, row({ status: 'in_transit' }), student).actions.cancel).toBe(false)
    expect(toDelivery(data, row({ status: 'queued' }), admin).actions.cancel).toBe(true)
    expect(toDelivery(data, row({ status: 'completed' }), admin).actions.reorder).toBe(false)
  })
})

describe('patrol view', () => {
  it('uses the patrol steps and allows cancel while underway', () => {
    const d = toDelivery(data, row({
      kind: 'patrol', stops: ['library', 'cafeteria'], rounds: 2, roundsDone: 1, status: 'underway', robotName: 'D01',
      events: [{ type: 'requested', at: 100 }, { type: 'assigned', at: 110 }],
    }), student)
    expect(d.timeline.map((s) => [s.key, s.state])).toEqual([['requested', 'done'], ['assigned', 'done'], ['patrolling', 'current']])
    expect(d.stops.map((s) => s.id)).toEqual(['library', 'cafeteria'])
    expect(d.actions.cancel).toBe(true)
  })
})

describe('demo seed', () => {
  it('starts one delivery with a robot on it and three scheduled ones', () => {
    const active = data.deliveries.filter((d) => d.plan && d.status !== 'completed')
    expect(active).toHaveLength(1)
    expect(data.robots.find((r) => r.name === active[0].robotName)?.deliveryId).toBe(active[0].id)
    expect(data.deliveries.filter((d) => d.status === 'scheduled')).toHaveLength(3)
  })
})
