
export type ParamType = 'float' | 'enum' | 'bool' | 'device' | 'text' | 'path'

export interface ParamDescriptor {
  name: string
  type: ParamType
  min: number
  max: number
  default: number | string
  unit: string
  curve: 'linear' | 'log'
  values?: string[]
  deviceRole?: 'capture' | 'playback'
  description?: string
}

export interface PortDescriptor {
  name: string
  type: string
}

export interface NodeDescriptor {
  type: string
  category: string
  label: string
  kind: 'dsp' | 'capture' | 'playback' | 'virtual_speaker' | 'virtual_mic'
  inputs: PortDescriptor[]
  outputs: PortDescriptor[]
  params: ParamDescriptor[]
  dynamicInputs: boolean
  dynamicOutputs: boolean
  latencyFrames: number
  extension: string
  realtimeSafe: boolean
  recommendedColdBlock: number
}

export interface ExtensionSetting {
  key: string
  label: string
  description: string
  type: 'bool' | 'int' | 'float' | 'text' | 'enum' | 'path'
  min: number
  max: number
  default: string
  values: string[]
}

export interface ExtensionPreset {
  name: string
  json: string
}

export type ExtensionState = 'loaded' | 'failed' | 'disabled' | 'pending'

export interface Extension {
  key: string
  path: string
  enabled: boolean
  disabledReason: string
  lastCrash: number
  values: Record<string, string>

  state: ExtensionState
  error: string

  id: string
  name: string
  version: string
  author: string
  description: string
  abiVersion: number
  nodeTypes: string[]
  settings: ExtensionSetting[]
  presets: ExtensionPreset[]
  hasUi: boolean
  uiDigest: string
}

export interface ExtensionUiManifest {
  key: string
  id: string
  entry: string
  digest: string
  nodeTypes: string[]
  assets: { path: string; mime: string }[]
}

export interface ExtensionUiEvent {
  extension: string
  event: string
  data: unknown
}

export interface ExtensionPresetRef {
  extension: string
  key: string
  name: string
}

export interface AudioDevice {
  id: number
  name: string
  description: string
  mediaClass: string
  application: string
  mediaName: string
  inputPorts: number
  outputPorts: number
}

export interface SpecNode {
  id: string
  type: string
  domain?: string
  params: Record<string, number | string>
  inputs?: number
  outputs?: number
  ui: { x: number; y: number }
}

export interface SpecEdge {
  from: { node: string; port: string | number }
  to: { node: string; port: string | number }
}

export interface SpecDomain {
  block?: number
  safety?: number
}

export interface GraphSpec {
  version: number
  nodes: SpecNode[]
  edges: SpecEdge[]
  domains?: Record<string, SpecDomain>
}

export interface DomainTelemetry {
  name: string
  cold: boolean
  block: number
  blockMs: number
  safety: number
  latencyFrames: number
  latencyMs: number
  stages: number
  passes: number
  underruns: number
  overruns: number
  dspUsAvg: number
  dspUs: number
  loadAvg: number
  load: number
}

export interface Telemetry {
  cycles: number
  xruns: number
  quantum: number
  sampleRate: number

  blockMs: number
  ioLatencyMs: number
  graphLatencyFrames: number
  graphLatencyMs: number
  totalLatencyMs: number
  jitterUs: number

  dspUs: number
  dspUsLast: number
  dspUsAvg: number
  load: number
  loadAvg: number

  profiling: boolean
  nodeCost: Record<string, number>
  nodeLatencyMs: Record<string, number>
  nodeStatus: Record<
    string,
    {
      state: 'offline' | 'loading' | 'ready' | 'degraded' | 'error'
      progress: number
      processedBlocks: number
      bypassedBlocks: number
      failures: number
      message: string
    }
  >

  realtime: boolean
  engine: 'up' | 'starting' | 'down'
  graphError: string
  generation: number
  swaps: number
  nodes: number
  bufferSlots: number
  droppedParams: number
  domains: DomainTelemetry[]
  outputLatencyMs: Record<string, number>
  meters: Record<string, { peak: number; rms: number }>

  scopes: Record<string, { wave: number[]; bands: number[] }>
  scopeBandsHz: number[]
}

async function json<T>(url: string, init?: RequestInit): Promise<T> {
  const res = await fetch(url, init)
  if (!res.ok) {
    const body = await res.text()
    let message = body
    try {
      message = JSON.parse(body).error ?? body
    } catch {
    }
    throw Object.assign(new Error(message), { status: res.status, body })
  }
  return res.status === 204 ? (undefined as T) : ((await res.json()) as T)
}

export const api = {
  descriptors: () => json<NodeDescriptor[]>('/api/descriptors'),
  graph: () => json<{ spec: GraphSpec }>('/api/graph'),
  presets: () =>
    json<{ presets: string[]; extensionPresets: ExtensionPresetRef[] }>('/api/presets'),
  audioDevices: () => json<AudioDevice[]>('/api/devices'),

  applyGraph: (spec: GraphSpec) =>
    json<void>('/api/graph', {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ spec }),
    }),

  setParam: (node: string, param: string, value: number) =>
    json<void>('/api/params', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ node, param, value }),
    }),

  savePreset: (name: string) =>
    json<void>(`/api/presets/${encodeURIComponent(name)}`, { method: 'PUT' }),

  loadPreset: (name: string) =>
    json<void>(`/api/presets/${encodeURIComponent(name)}/load`, {
      method: 'POST',
    }),

  deletePreset: (name: string) =>
    json<void>(`/api/presets/${encodeURIComponent(name)}`, { method: 'DELETE' }),

  setProfiling: (enabled: boolean) =>
    json<void>('/api/profiling', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ enabled }),
    }),

  resetStats: () => json<void>('/api/stats/reset', { method: 'POST' }),

  restartEngine: () => json<void>('/api/engine/restart', { method: 'POST' }),

  extensions: () => json<Extension[]>('/api/extensions'),

  rescanExtensions: () => json<Extension[]>('/api/extensions/rescan', { method: 'POST' }),

  setExtensionEnabled: (key: string, enabled: boolean) =>
    json<Extension[]>(
      `/api/extensions/${encodeURIComponent(key)}/${enabled ? 'enable' : 'disable'}`,
      { method: 'POST' },
    ),

  saveExtensionSettings: (key: string, values: Record<string, string>) =>
    json<void>(`/api/extensions/${encodeURIComponent(key)}/settings`, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ values }),
    }),

  deleteExtension: (key: string) =>
    json<Extension[]>(`/api/extensions/${encodeURIComponent(key)}`, { method: 'DELETE' }),

  installExtension: (file: File) =>
    json<Extension[]>(`/api/extensions/install?name=${encodeURIComponent(file.name)}`, {
      method: 'POST',
      body: file,
    }),

  loadExtensionPreset: (key: string, name: string) =>
    json<void>(
      `/api/presets/ext/${encodeURIComponent(key)}/${encodeURIComponent(name)}/load`,
      { method: 'POST' },
    ),

  extensionUiManifest: (key: string) =>
    json<ExtensionUiManifest>(`/api/extensions/${encodeURIComponent(key)}/ui/manifest`),

  callExtensionUi: (key: string, method: string, data: unknown) =>
    json<{ data: unknown }>(`/api/extensions/${encodeURIComponent(key)}/ui/call`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ method, data }),
    }).then((reply) => reply.data),
}

export function extensionUiAssetUrl(
  key: string,
  digest: string,
  path: string,
): string {
  const encodedPath = path.split('/').map(encodeURIComponent).join('/')
  return `/api/extensions/${encodeURIComponent(key)}/ui/assets/${encodeURIComponent(digest)}/${encodedPath}`
}

export function subscribeExtensionEvents(
  onData: (event: ExtensionUiEvent) => void,
): () => void {
  const source = new EventSource('/api/extensions/ui/events')
  const listener = (event: Event) => {
    try {
      onData(JSON.parse((event as MessageEvent<string>).data) as ExtensionUiEvent)
    } catch {
    }
  }
  source.addEventListener('extension', listener)
  return () => source.close()
}

export function subscribeTelemetry(onData: (t: Telemetry) => void): () => void {
  const source = new EventSource('/api/events')
  source.onmessage = (event) => {
    try {
      onData(JSON.parse(event.data) as Telemetry)
    } catch {
    }
  }
  return () => source.close()
}
