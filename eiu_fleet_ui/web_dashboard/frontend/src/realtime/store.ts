import { create } from 'zustand'
import type { RmfLink, RobotLive, ServerMessage } from '../api/types'
import type { TransportStatus } from './transport'

interface LiveState {
  status: TransportStatus
  rmf: RmfLink | null
  /** Robots by name; a patch replaces only the robots it names. */
  robots: Record<string, RobotLive>
  seq: number
  periodMs: number
  receivedAt: number
  setStatus: (s: TransportStatus) => void
  setRmf: (link: RmfLink) => void
  applyPatch: (m: Extract<ServerMessage, { type: 'patch' }>) => void
  clear: () => void
}

export const useLive = create<LiveState>((set) => ({
  status: 'closed',
  rmf: null,
  robots: {},
  seq: 0,
  periodMs: 500,
  receivedAt: 0,
  setStatus: (status) => set({ status }),
  setRmf: (rmf) => set({ rmf }),
  applyPatch: (m) =>
    set((s) => {
      const robots = { ...s.robots }
      for (const name of m.remove) delete robots[name]
      for (const robot of m.upsert) robots[robot.name] = robot
      return { robots, seq: m.seq, periodMs: m.periodMs, receivedAt: performance.now() }
    }),
  clear: () => set({ robots: {}, seq: 0, rmf: null }),
}))

export const useRobot = (name: string | null | undefined) =>
  useLive((s) => (name ? s.robots[name] : undefined))
