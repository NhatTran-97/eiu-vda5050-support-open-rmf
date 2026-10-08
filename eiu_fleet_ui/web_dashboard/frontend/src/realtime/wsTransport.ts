import type { ServerMessage } from '../api/types'
import type { Transport } from './transport'

const RETRY_MIN_MS = 1000
const RETRY_MAX_MS = 15000

/** The backend's /ws stream on the page's own origin; reconnects with exponential backoff. */
export const wsTransport: Transport = {
  connect({ onMessage, onStatus }) {
    let socket: WebSocket | null = null
    let retryMs = RETRY_MIN_MS
    let timer: number | undefined
    let closed = false

    const open = () => {
      onStatus('connecting')
      const scheme = location.protocol === 'https:' ? 'wss' : 'ws'
      socket = new WebSocket(`${scheme}://${location.host}/ws`)
      socket.onopen = () => {
        retryMs = RETRY_MIN_MS
        onStatus('open')
      }
      socket.onmessage = (e) => {
        try {
          onMessage(JSON.parse(String(e.data)) as ServerMessage)
        } catch {
          // Ignore a malformed frame.
        }
      }
      socket.onclose = () => {
        socket = null
        if (closed) return
        onStatus('closed')
        timer = window.setTimeout(open, retryMs)
        retryMs = Math.min(retryMs * 2, RETRY_MAX_MS)
      }
    }

    open()
    return () => {
      closed = true
      window.clearTimeout(timer)
      socket?.close()
      onStatus('closed')
    }
  },
}
