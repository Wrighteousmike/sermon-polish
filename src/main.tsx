import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import '@fontsource-variable/plus-jakarta-sans'
import './index.css'
import App from './App.tsx'
import './theme.css'

async function start() {
  // Development only: ?mock=<screen> swaps in a fake engine (see mock.ts); stripped from release builds.
  if (import.meta.env.DEV && new URLSearchParams(window.location.search).has('mock')) {
    ;(await import('./mock')).installMocks()
  }
  render()
}

function render() {
  createRoot(document.getElementById('root')!).render(
    <StrictMode>
      <App />
    </StrictMode>,
  )
}

void start()
