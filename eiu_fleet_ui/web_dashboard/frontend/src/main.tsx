import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import { App } from './app/App'
import { USE_MOCKS } from './app/env'
import './i18n'
import { startTheme } from './lib/theme'
import './styles/index.css'

async function bootstrap() {
  startTheme()
  if (USE_MOCKS) {
    const { startMocks } = await import('./mocks/browser')
    await startMocks()
  }
  createRoot(document.getElementById('root')!).render(
    <StrictMode>
      <App />
    </StrictMode>,
  )
}

void bootstrap()
