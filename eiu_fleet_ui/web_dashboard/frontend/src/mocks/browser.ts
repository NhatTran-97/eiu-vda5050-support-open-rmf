// Starts the in-browser demo backend.
import { setupWorker } from 'msw/browser'
import { watchOtherTabs } from './db'
import { handlers } from './handlers'
import { markChanged, startSimulator } from './simulator'

export async function startMocks(): Promise<void> {
  const worker = setupWorker(...handlers)
  await worker.start({ onUnhandledRequest: 'bypass', quiet: true })
  watchOtherTabs(() => markChanged('deliveries', 'notifications'))
  startSimulator()
}
