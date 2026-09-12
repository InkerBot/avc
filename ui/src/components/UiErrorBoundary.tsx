import { Component, type ErrorInfo, type ReactNode } from 'react'
import i18n from '../i18n'

interface UiErrorBoundaryProps {
  children: ReactNode
}

interface UiErrorBoundaryState {
  error: Error | null
}

export class UiErrorBoundary extends Component<UiErrorBoundaryProps, UiErrorBoundaryState> {
  state: UiErrorBoundaryState = { error: null }

  static getDerivedStateFromError(error: Error): UiErrorBoundaryState {
    return { error }
  }

  componentDidCatch(error: Error, info: ErrorInfo) {
    console.error('AVC UI render failed', error, info.componentStack)
  }

  render() {
    if (!this.state.error) return this.props.children

    return (
      <div className="boot boot--error" role="alert">
        <h1>{i18n.t('app.uiErrorTitle')}</h1>
        <p>{i18n.t('app.uiErrorHint')}</p>
        <code className="boot__detail">{this.state.error.message}</code>
        <button className="button button--primary" onClick={() => window.location.reload()}>
          {i18n.t('app.reload')}
        </button>
      </div>
    )
  }
}
