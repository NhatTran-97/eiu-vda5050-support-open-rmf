// Time-based plan of one delivery: drive to the pickup, wait for loading, drive to the drop-off, wait for collection.
// Every stage and pose is a function of time, so a reloaded page resumes where the robot would be.
import { length, pointAt, remainder, type Pt } from './geometry'

export interface Plan {
  startAt: number
  toPickup: Pt[]
  toDropoff: Pt[]
  pickupArriveAt: number
  departAt: number
  arrivingAt: number
  arriveAt: number
  completeAt: number
  batteryStart: number
}

export interface PlanTiming {
  speedMps: number
  loadingDwellS: number
  collectDwellS: number
  arrivingSoonS: number
}

export type PlanStage = 'to_pickup' | 'at_pickup' | 'in_transit' | 'arrived' | 'completed'

export type Milestone = 'arrived_pickup' | 'picked_up' | 'arriving' | 'arrived' | 'completed'

export function makePlan(toPickup: Pt[], toDropoff: Pt[], startAt: number, t: PlanTiming, batteryStart: number): Plan {
  const pickupArriveAt = startAt + (length(toPickup) / t.speedMps) * 1000
  const departAt = pickupArriveAt + t.loadingDwellS * 1000
  const arriveAt = departAt + (length(toDropoff) / t.speedMps) * 1000
  return {
    startAt,
    toPickup,
    toDropoff,
    pickupArriveAt,
    departAt,
    arrivingAt: Math.max(departAt, arriveAt - t.arrivingSoonS * 1000),
    arriveAt,
    completeAt: arriveAt + t.collectDwellS * 1000,
    batteryStart,
  }
}

export function milestones(plan: Plan): [Milestone, number][] {
  return [
    ['arrived_pickup', plan.pickupArriveAt],
    ['picked_up', plan.departAt],
    ['arriving', plan.arrivingAt],
    ['arrived', plan.arriveAt],
    ['completed', plan.completeAt],
  ]
}

export function stageAt(plan: Plan, t: number): PlanStage {
  if (t < plan.pickupArriveAt) return 'to_pickup'
  if (t < plan.departAt) return 'at_pickup'
  if (t < plan.arriveAt) return 'in_transit'
  if (t < plan.completeAt) return 'arrived'
  return 'completed'
}

function fraction(t: number, from: number, to: number): number {
  return to <= from ? 1 : Math.min(1, Math.max(0, (t - from) / (to - from)))
}

export interface PlanPose extends Pt {
  yaw: number
  traveledM: number
  remainingM: number
  path: Pt[]
  moving: boolean
}

export function poseAt(plan: Plan, t: number): PlanPose {
  const d1 = length(plan.toPickup)
  const d2 = length(plan.toDropoff)
  const stage = stageAt(plan, t)
  if (stage === 'to_pickup') {
    const d = d1 * fraction(t, plan.startAt, plan.pickupArriveAt)
    const p = pointAt(plan.toPickup, d)
    return { ...p, traveledM: d, remainingM: d1 - d + d2, path: [...remainder(plan.toPickup, d), ...plan.toDropoff.slice(1)], moving: true }
  }
  if (stage === 'at_pickup') {
    const p = pointAt(plan.toDropoff, 0)
    return { ...p, yaw: pointAt(plan.toPickup, d1).yaw, traveledM: d1, remainingM: d2, path: plan.toDropoff, moving: false }
  }
  if (stage === 'in_transit') {
    const d = d2 * fraction(t, plan.departAt, plan.arriveAt)
    const p = pointAt(plan.toDropoff, d)
    return { ...p, traveledM: d1 + d, remainingM: d2 - d, path: remainder(plan.toDropoff, d), moving: true }
  }
  const end = pointAt(plan.toDropoff, d2)
  return { ...end, traveledM: d1 + d2, remainingM: 0, path: [], moving: false }
}

export function batteryAt(plan: Plan, t: number, drainPerM: number): number {
  return Math.max(0, plan.batteryStart - poseAt(plan, t).traveledM * drainPerM)
}
