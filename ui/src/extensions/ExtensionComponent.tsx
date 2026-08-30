import { useEffect, useRef, useState, type ReactNode } from 'react'

import type { Extension } from '../api'
import { extensionRuntime, type ExtensionElementContext, type ExtensionSurface } from './runtime'

export function ExtensionComponent({
  extension,
  surface,
  nodeType,
  context,
  fallback,
}: {
  extension: Extension
  surface: ExtensionSurface
  nodeType?: string
  context: ExtensionElementContext
  fallback?: ReactNode
}) {
  const host = useRef<HTMLDivElement>(null)
  const [tag, setTag] = useState<string | null>(null)
  const [error, setError] = useState('')

  useEffect(() => {
    let active = true
    setTag(null)
    setError('')
    void extensionRuntime.load(extension).then((registration) => {
      if (!active || !registration) return
      setTag(extensionRuntime.tag(registration, surface, nodeType))
    }).catch((reason: unknown) => {
      if (active) setError(reason instanceof Error ? reason.message : String(reason))
    })
    return () => { active = false }
  }, [extension, nodeType, surface])

  useEffect(() => {
    const root = host.current
    if (!root || !tag) return
    const element = document.createElement(tag)
    element.avcContext = context
    root.replaceChildren(element)
    return () => element.remove()
  }, [context, tag])

  if (!tag) {
    return (
      <>
        {error && <p className="banner banner--error" role="status">{error}</p>}
        {fallback}
      </>
    )
  }

  return <div ref={host} className={`extension-component extension-component--${surface}`} />
}
