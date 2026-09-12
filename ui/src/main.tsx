import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import App from './App'
import { UiErrorBoundary } from './components/UiErrorBoundary'
import './i18n'
import './index.css'

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <UiErrorBoundary>
      <App />
    </UiErrorBoundary>
  </StrictMode>,
)
