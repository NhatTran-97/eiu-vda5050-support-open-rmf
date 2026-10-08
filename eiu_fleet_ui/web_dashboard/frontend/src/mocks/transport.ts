// Realtime stream of the demo backend: keyed patches of the robots the viewer may see, plus change events.
import type { RobotLive } from '../api/types'
import type { Transport } from '../realtime/transport'
import { db } from './db'
import { subscribe } from './simulator'
import { can, fleetVisible } from './access'
import { ACTIVE } from './views'

export const mockTransport: Transport = {
  connect({ onMessage, onStatus }) {
    const sent = new Map<string, string>()
    let seq = 0
    onStatus('open')
    onMessage({ type: 'system', rmf: 'online' })

    const unsubscribe = subscribe((frame) => {
      const data = db()
      const user = data.session && data.users.find((u) => u.id === data.session!.userId)
      if (!user) return
      const own = new Set(data.deliveries.filter((d) => d.requesterId === user.id && ACTIVE.has(d.status)).map((d) => d.id))
      const visible = can(user, 'fleet.view')
        ? frame.robots.filter((r) => fleetVisible(user, r.fleet))
        : frame.robots.filter((r) => r.deliveryId !== null && own.has(r.deliveryId))

      const upsert: RobotLive[] = []
      const seen = new Set<string>()
      for (const robot of visible) {
        seen.add(robot.name)
        const encoded = JSON.stringify(robot)
        if (sent.get(robot.name) !== encoded) {
          sent.set(robot.name, encoded)
          upsert.push(robot)
        }
      }
      const remove = [...sent.keys()].filter((name) => !seen.has(name))
      remove.forEach((name) => sent.delete(name))

      if (upsert.length || remove.length) {
        onMessage({ type: 'patch', topic: 'robots', seq: ++seq, periodMs: frame.periodMs, upsert, remove })
      }
      if (frame.topics.length) onMessage({ type: 'event', topics: frame.topics })
    })

    return () => {
      unsubscribe()
      onStatus('closed')
    }
  },
}
