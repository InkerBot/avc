import {
  api,
  extensionUiAssetUrl,
  subscribeExtensionEvents,
  type Extension,
  type ExtensionUiEvent,
  type ExtensionUiManifest,
} from '../api'

export type ExtensionSurface = 'settings' | 'node-inspector' | 'node-body'

interface Registration {
  manifest: ExtensionUiManifest
  settings?: string
  nodeInspector: Map<string, string>
  nodeBody: Map<string, string>
}

export interface ExtensionElementContext {
  extension: Extension
  call(method: string, data?: unknown): Promise<unknown>
  on(event: string, listener: (data: unknown) => void): () => void
  settings: {
    get(): Record<string, string>
    save(values: Record<string, string>): Promise<void>
  }
  node?: {
    id: string
    type: string
    params: Record<string, number>
    options: Record<string, string>
    setParam(name: string, value: number): void
    setOption(name: string, value: string): void
  }
}

declare global {
  interface HTMLElement {
    avcContext?: ExtensionElementContext
  }
}

type Listener = (data: unknown) => void

class ExtensionRuntime {
  private registrations = new Map<string, Registration>()
  private loading = new Map<string, Promise<Registration | null>>()
  private listeners = new Map<string, Set<Listener>>()
  private stopEvents: (() => void) | null = null

  load(extension: Extension): Promise<Registration | null> {
    if (!extension.hasUi || extension.state !== 'loaded') return Promise.resolve(null)
    const current = this.registrations.get(extension.key)
    if (current?.manifest.digest === extension.uiDigest) return Promise.resolve(current)
    if (current) this.registrations.delete(extension.key)
    const inflight = this.loading.get(extension.key)
    if (inflight) return inflight

    const promise = this.activate(extension).finally(() => this.loading.delete(extension.key))
    this.loading.set(extension.key, promise)
    return promise
  }

  tag(registration: Registration, surface: ExtensionSurface, nodeType?: string): string | null {
    if (surface === 'settings') return registration.settings ?? null
    if (!nodeType) return null
    return (surface === 'node-inspector'
      ? registration.nodeInspector.get(nodeType)
      : registration.nodeBody.get(nodeType)) ?? null
  }

  on(extensionId: string, event: string, listener: Listener): () => void {
    this.ensureEvents()
    const key = `${extensionId}\u0000${event}`
    const listeners = this.listeners.get(key) ?? new Set<Listener>()
    listeners.add(listener)
    this.listeners.set(key, listeners)
    return () => {
      listeners.delete(listener)
      if (listeners.size === 0) this.listeners.delete(key)
    }
  }

  private ensureEvents() {
    if (this.stopEvents) return
    this.stopEvents = subscribeExtensionEvents((message: ExtensionUiEvent) => {
      this.listeners.get(`${message.extension}\u0000${message.event}`)?.forEach((listener) => {
        try {
          listener(message.data)
        } catch {
        }
      })
    })
  }

  private async activate(extension: Extension): Promise<Registration> {
    const manifest = await api.extensionUiManifest(extension.key)
    const registration: Registration = {
      manifest,
      nodeInspector: new Map(),
      nodeBody: new Map(),
    }
    const acceptTag = (tagName: string): string => {
      if (!/^[a-z][a-z0-9._-]*-[a-z0-9._-]+$/.test(tagName)) {
        throw new Error(`Invalid custom element name: ${tagName}`)
      }
      return tagName
    }
    const acceptNode = (nodeType: string): void => {
      if (!manifest.nodeTypes.includes(nodeType)) {
        throw new Error(`Extension ${manifest.id} cannot manage node type ${nodeType}`)
      }
    }
    const scopedApi = Object.freeze({
      extension: Object.freeze({ key: extension.key, id: manifest.id }),
      call: (method: string, data: unknown = null) => api.callExtensionUi(extension.key, method, data),
      on: (event: string, listener: Listener) => this.on(manifest.id, event, listener),
      components: Object.freeze({
        registerSettings: (tagName: string) => { registration.settings = acceptTag(tagName) },
        registerNodeInspector: (nodeType: string, tagName: string) => {
          acceptNode(nodeType)
          registration.nodeInspector.set(nodeType, acceptTag(tagName))
        },
        registerNodeBody: (nodeType: string, tagName: string) => {
          acceptNode(nodeType)
          registration.nodeBody.set(nodeType, acceptTag(tagName))
        },
      }),
    })

    const url = extensionUiAssetUrl(extension.key, manifest.digest, manifest.entry)
    const module = await import(/* @vite-ignore */ url) as { activate?: (api: typeof scopedApi) => void | Promise<void> }
    if (typeof module.activate !== 'function') throw new Error('Extension UI module exports no activate(api) function')
    await module.activate(scopedApi)
    this.registrations.set(extension.key, registration)
    return registration
  }
}

export const extensionRuntime = new ExtensionRuntime()
