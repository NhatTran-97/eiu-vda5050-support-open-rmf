import type { ServerMessage } from '../api/types'

export type TransportStatus = 'connecting' | 'open' | 'closed'

/** A source of server messages; connect() returns the function that disconnects. */
export interface Transport {
  connect(handlers: { onMessage: (m: ServerMessage) => void; onStatus: (s: TransportStatus) => void }): () => void
}
