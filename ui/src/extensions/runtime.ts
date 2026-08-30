import {
  api,
  extensionUiAssetUrl,
  subscribeExtensionEvents,
  type Extension,
  type ExtensionUiEvent,
  type ExtensionUiManifest,
} from '../api'
import { extensionI18n, removeExtensionI18n, type ExtensionI18n } from '../i18n'

export type ExtensionSurface = 'settings' | 'node-inspector' | 'node-body'

interface Registration {
  manifest: ExtensionUiManifest
  settings?: string
  nodeInspector: Map<string, string>
  nodeBody: Map<string, string>
}

export interface ExtensionElementContext {
  extension: Extension
  i18n: ExtensionI18n
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

export interface ExtensionActivationApi {
  readonly extension: Readonly<{ key: string; id: string }>
  readonly i18n: ExtensionI18n
  call(method: string, data?: unknown): Promise<unknown>
  on(event: string, listener: Listener): () => void
  readonly components: Readonly<{
    registerSettings(tagName: string): void
    registerNodeInspector(nodeType: string, tagName: string): void
    registerNodeBody(nodeType: string, tagName: string): void
  }>
}

class ExtensionRuntime {
  private registrations = new Map<string, Registration>()
  private loading = new Map<string, {
    digest: string
    promise: Promise<Registration | null>
  }>()
  private desiredDigests = new Map<string, string>()
  private listeners = new Map<string, Set<Listener>>()
  private stopEvents: (() => void) | null = null

  async sync(extensions: Extension[]): Promise<void> {
    const available = new Map(
      extensions
        .filter((extension) => extension.hasUi && extension.state === 'loaded')
        .map((extension) => [extension.key, extension]),
    )
    for (const key of this.desiredDigests.keys()) {
      if (!available.has(key)) this.unload(key)
    }
    for (const [key, registration] of this.registrations) {
      const extension = available.get(key)
      if (!extension || extension.uiDigest !== registration.manifest.digest) this.unload(key)
    }
    await Promise.allSettled([...available.values()].map((extension) => this.load(extension)))
  }

  load(extension: Extension): Promise<Registration | null> {
    if (!extension.hasUi || extension.state !== 'loaded') {
      this.unload(extension.key)
      return Promise.resolve(null)
    }
    const current = this.registrations.get(extension.key)
    if (current?.manifest.digest === extension.uiDigest) {
      this.desiredDigests.set(extension.key, extension.uiDigest)
      return Promise.resolve(current)
    }
    if (current) this.unload(extension.key)
    this.desiredDigests.set(extension.key, extension.uiDigest)
    const inflight = this.loading.get(extension.key)
    if (inflight?.digest === extension.uiDigest) return inflight.promise
    if (inflight) {
      return inflight.promise
        .catch(() => null)
        .then(() => this.load(extension))
    }

    const promise = this.activate(extension).finally(() => {
      if (this.loading.get(extension.key)?.promise === promise) {
        this.loading.delete(extension.key)
      }
    })
    this.loading.set(extension.key, { digest: extension.uiDigest, promise })
    return promise
  }

  i18n(extensionId: string): ExtensionI18n {
    return extensionI18n(extensionId)
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

  private unload(key: string) {
    const current = this.registrations.get(key)
    if (current) removeExtensionI18n(current.manifest.id)
    this.registrations.delete(key)
    this.desiredDigests.delete(key)
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
    const scopedApi: ExtensionActivationApi = Object.freeze({
      extension: Object.freeze({ key: extension.key, id: manifest.id }),
      i18n: extensionI18n(manifest.id),
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

    try {
      const url = extensionUiAssetUrl(extension.key, manifest.digest, manifest.entry)
      const module = await import(/* @vite-ignore */ url) as {
        activate?: (api: ExtensionActivationApi) => void | Promise<void>
      }
      if (typeof module.activate !== 'function') {
        throw new Error('Extension UI module exports no activate(api) function')
      }
      await module.activate(scopedApi)
    } catch (error) {
      removeExtensionI18n(manifest.id)
      throw error
    }
    if (this.desiredDigests.get(extension.key) === manifest.digest) {
      this.registrations.set(extension.key, registration)
    } else {
      removeExtensionI18n(manifest.id)
    }
    return registration
  }
}

export const extensionRuntime = new ExtensionRuntime()
