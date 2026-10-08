import { useQueryClient } from '@tanstack/react-query'
import { useEffect, type ReactNode } from 'react'
import { invalidateTopic, useMe } from '../api/queries'
import { USE_MOCKS } from '../app/env'
import { useLive } from './store'
import type { Transport } from './transport'

async function loadTransport(): Promise<Transport | null> {
  if (USE_MOCKS) return (await import('../mocks/transport')).mockTransport
  return (await import('./wsTransport')).wsTransport
}

/** Keeps one realtime connection open while a user is signed in. */
export function RealtimeProvider({ children }: { children: ReactNode }) {
  const client = useQueryClient()
  const userId = useMe().data?.id

  useEffect(() => {
    if (!userId) return
    let disconnect: (() => void) | undefined
    let cancelled = false
    const live = useLive.getState()
    live.setStatus('connecting')
    void loadTransport().then((transport) => {
      if (cancelled) return
      if (!transport) {
        live.setStatus('closed')
        return
      }
      disconnect = transport.connect({
        onStatus: (s) => useLive.getState().setStatus(s),
        onMessage: (m) => {
          if (m.type === 'patch') useLive.getState().applyPatch(m)
          else if (m.type === 'system') useLive.getState().setRmf(m.rmf)
          else m.topics.forEach((t) => invalidateTopic(client, t))
        },
      })
    })
    return () => {
      cancelled = true
      disconnect?.()
      useLive.getState().clear()
    }
  }, [userId, client])

  return children
}
