import { create } from 'zustand'
import {
  addEdge,
  applyEdgeChanges,
  applyNodeChanges,
  type Connection,
  type Edge,
  type EdgeChange,
  type Node,
  type NodeChange,
} from '@xyflow/react'
import {
  api,
  type AudioDevice,
  type Extension,
  type ExtensionPresetRef,
  type GraphSpec,
  type NodeDescriptor,
  type PortDescriptor,
  type SpecDomain,
  type SpecNode,
  type Telemetry,
} from './api'

export interface AvcNodeData extends Record<string, unknown> {
  type: string
  params: Record<string, number>
  options: Record<string, string>
  inputs?: number
  outputs?: number
  domain?: string
}

export const COLD = 'cold'

export const COLD_BLOCKS = [64, 128, 256, 512, 1024, 2048, 4096, 8192, 12000, 16000, 24000, 32768]

export type DomainNameIssue = 'empty' | 'reserved' | 'tooLong' | 'control' | 'exists'

export function domainNameIssue(
  raw: string,
  domains: Record<string, SpecDomain>,
  current?: string,
): DomainNameIssue | null {
  const name = raw.trim()
  if (!name) return 'empty'
  if (name === 'hot') return 'reserved'
  if (name.length > 48) return 'tooLong'
  if (/[/\u0000-\u001f\u007f]/.test(name)) return 'control'
  if (name !== current && Object.hasOwn(domains, name)) return 'exists'
  return null
}

function pruneDomains(
  domains: Record<string, SpecDomain>,
  nodes: AvcNode[],
): Record<string, SpecDomain> {
  const used = new Set(nodes.flatMap((node) => (node.data.domain ? [node.data.domain] : [])))
  return Object.fromEntries(Object.entries(domains).filter(([name]) => used.has(name)))
}

export type AvcNode = Node<AvcNodeData>

export function portNames(declared: { name: string }[], count: number, prefix: string): string[] {
  return Array.from({ length: count }, (_, i) =>
    i < declared.length ? declared[i].name : `${prefix}_${i + 1}`,
  )
}

export const AUDIO_PORT_TYPE = 'audio'

export function portTypes(declared: PortDescriptor[], count: number): string[] {
  const last = declared.length > 0 ? declared[declared.length - 1].type : ''
  return Array.from({ length: count }, (_, i) =>
    (i < declared.length ? declared[i].type : last) || AUDIO_PORT_TYPE,
  )
}

export function portCounts(descriptor: NodeDescriptor, data: AvcNodeData) {
  return {
    inputs: descriptor.dynamicInputs && data.inputs ? data.inputs : descriptor.inputs.length,
    outputs: descriptor.dynamicOutputs && data.outputs ? data.outputs : descriptor.outputs.length,
  }
}

export function portTypeAt(
  nodes: AvcNode[],
  descriptors: Record<string, NodeDescriptor>,
  nodeId: string | null,
  handle: string | null,
): string | undefined {
  const node = nodes.find((n) => n.id === nodeId)
  const descriptor = node ? descriptors[node.data.type] : undefined
  if (!node || !descriptor || !handle) return undefined

  const side = handle.startsWith('out:') ? 'out' : 'in'
  const name = handle.slice(side.length + 1)
  const counts = portCounts(descriptor, node.data)
  const declared = side === 'out' ? descriptor.outputs : descriptor.inputs
  const index = portNames(declared, side === 'out' ? counts.outputs : counts.inputs, side).indexOf(
    name,
  )
  if (index < 0) return undefined
  return portTypes(declared, side === 'out' ? counts.outputs : counts.inputs)[index]
}

function specToFlow(spec: GraphSpec): { nodes: AvcNode[]; edges: Edge[] } {
  const nodes: AvcNode[] = spec.nodes.map((n) => {
    const params: Record<string, number> = {}
    const options: Record<string, string> = {}
    Object.entries(n.params ?? {}).forEach(([key, value]) => {
      if (typeof value === 'string') options[key] = value
      else params[key] = value
    })

    return {
      id: n.id,
      type: 'avc',
      position: { x: n.ui?.x ?? 0, y: n.ui?.y ?? 0 },
      data: {
        type: n.type,
        params,
        options,
        inputs: n.inputs,
        outputs: n.outputs,
        domain: n.domain || undefined,
      },
    }
  })

  const edges: Edge[] = spec.edges.map((e, i) => ({
    id: `e${i}-${e.from.node}-${e.to.node}`,
    source: e.from.node,
    sourceHandle: `out:${e.from.port}`,
    target: e.to.node,
    targetHandle: `in:${e.to.port}`,
  }))
  return { nodes, edges }
}

function flowToSpec(
  nodes: AvcNode[],
  edges: Edge[],
  version: number,
  domains: Record<string, SpecDomain>,
): GraphSpec {
  const specNodes: SpecNode[] = nodes.map((n) => {
    const params: Record<string, number | string> = { ...n.data.params }
    Object.entries(n.data.options).forEach(([key, value]) => {
      if (value) params[key] = value
    })

    const node: SpecNode = {
      id: n.id,
      type: n.data.type,
      params,
      ui: { x: Math.round(n.position.x), y: Math.round(n.position.y) },
    }
    if (n.data.inputs) node.inputs = n.data.inputs
    if (n.data.outputs) node.outputs = n.data.outputs
    if (n.data.domain) node.domain = n.data.domain
    return node
  })

  const used: Record<string, SpecDomain> = {}
  nodes.forEach((n) => {
    if (n.data.domain && domains[n.data.domain]) used[n.data.domain] = domains[n.data.domain]
  })

  return {
    version,
    nodes: specNodes,
    domains: used,
    edges: edges.map((e) => ({
      from: { node: e.source, port: (e.sourceHandle ?? 'out:out').slice(4) },
      to: { node: e.target, port: (e.targetHandle ?? 'in:in').slice(3) },
    })),
  }
}

export type View = 'editor' | 'extensions'

interface State {
  view: View
  descriptors: Record<string, NodeDescriptor>
  palette: NodeDescriptor[]
  audioDevices: AudioDevice[]
  presets: string[]
  extensionPresets: ExtensionPresetRef[]
  extensions: Extension[]
  selectedExtension: string | null
  extensionsBusy: boolean
  extensionError: string | null
  nodes: AvcNode[]
  edges: Edge[]
  domains: Record<string, SpecDomain>
  dirty: boolean
  error: string | null
  telemetry: Telemetry | null

  scopes: Record<string, { wave: number[]; bands: number[] }>
  selected: string | null

  setView: (view: View) => void
  load: () => Promise<void>
  refreshDescriptors: () => Promise<void>
  apply: () => Promise<void>
  revert: () => Promise<void>
  addNode: (type: string, at: { x: number; y: number }) => void
  setParam: (id: string, name: string, value: number) => void
  setPortCount: (id: string, side: 'inputs' | 'outputs', count: number) => void
  setOption: (id: string, name: string, value: string) => void
  setDomain: (id: string, domain: string | undefined) => void
  renameDomain: (domain: string, next: string) => void
  setDomainSetting: (domain: string, key: keyof SpecDomain, value: number) => void
  onNodesChange: (changes: NodeChange<AvcNode>[]) => void
  onEdgesChange: (changes: EdgeChange[]) => void
  canConnect: (connection: Connection | Edge) => boolean
  onConnect: (connection: Connection) => void
  select: (id: string | null) => void
  setTelemetry: (t: Telemetry) => void
  setProfiling: (on: boolean) => Promise<void>
  resetStats: () => Promise<void>
  refreshPresets: () => Promise<void>
  savePreset: (name: string) => Promise<void>
  loadPreset: (name: string) => Promise<void>
  loadExtensionPreset: (key: string, name: string) => Promise<void>

  refreshExtensions: () => Promise<void>
  rescanExtensions: () => Promise<void>
  selectExtension: (key: string | null) => void
  setExtensionEnabled: (key: string, enabled: boolean) => Promise<void>
  saveExtensionSettings: (key: string, values: Record<string, string>) => Promise<void>
  installExtension: (file: File) => Promise<void>
  deleteExtension: (key: string) => Promise<void>
  restartEngine: () => Promise<void>
}

let nextId = 1

export const useStore = create<State>((set, get) => ({
  view: 'editor',
  descriptors: {},
  palette: [],
  audioDevices: [],
  presets: [],
  extensionPresets: [],
  extensions: [],
  selectedExtension: null,
  extensionsBusy: false,
  extensionError: null,
  nodes: [],
  edges: [],
  domains: {},
  dirty: false,
  error: null,
  telemetry: null,
  scopes: {},
  selected: null,

  setView: (view) => set({ view }),

  refreshDescriptors: async () => {
    const descriptors = await api.descriptors()
    const byType: Record<string, NodeDescriptor> = {}
    descriptors.forEach((d) => (byType[d.type] = d))
    set({ descriptors: byType, palette: descriptors })
    void get().refreshExtensions()
    void get().refreshPresets()
  },

  load: async () => {
    const [descriptors, graph, audioDevices] = await Promise.all([
      api.descriptors(),
      api.graph(),
      api.audioDevices(),
    ])
    const byType: Record<string, NodeDescriptor> = {}
    descriptors.forEach((d) => (byType[d.type] = d))
    const { nodes, edges } = specToFlow(graph.spec)
    set({
      descriptors: byType,
      palette: descriptors,
      audioDevices,
      nodes,
      edges,
      domains: graph.spec.domains ?? {},
      dirty: false,
      error: null,
      scopes: {},
    })
    void get().refreshPresets()
    void get().refreshExtensions()
  },

  apply: async () => {
    const { nodes, edges, domains } = get()
    try {
      const spec = flowToSpec(nodes, edges, 1, domains)
      await api.applyGraph(spec)
      set({ dirty: false, error: null })
    } catch (err) {
      set({ error: (err as Error).message })
    }
  },

  revert: async () => {
    await get().load()
  },

  addNode: (type, at) => {
    const descriptor = get().descriptors[type]
    if (!descriptor) return
    const params: Record<string, number> = {}
    const options: Record<string, string> = {}
    descriptor.params.forEach((p) => {
      if (p.type === 'device' || p.type === 'text' || p.type === 'path') {
        options[p.name] = typeof p.default === 'string' ? p.default : ''
      } else {
        params[p.name] = typeof p.default === 'number' ? p.default : 0
      }
    })

    const id = `${type}_${nextId++}`
    const node: AvcNode = {
      id,
      type: 'avc',
      position: at,
      data: {
        type,
        params,
        options,
        inputs: descriptor.dynamicInputs ? descriptor.inputs.length : undefined,
        outputs: descriptor.dynamicOutputs ? descriptor.outputs.length : undefined,
        domain: descriptor.realtimeSafe ? undefined : COLD,
      },
    }
    set((s) => {
      let domains = s.domains
      if (!descriptor.realtimeSafe && descriptor.recommendedColdBlock > 0) {
        const current = domains[COLD] ?? {}
        if ((current.block ?? 0) < descriptor.recommendedColdBlock) {
          domains = {
            ...domains,
            [COLD]: { ...current, block: descriptor.recommendedColdBlock },
          }
        }
      }
      return { nodes: [...s.nodes, node], domains, selected: id, dirty: true }
    })
  },

  setParam: (id, name, value) => {
    set((s) => ({
      nodes: s.nodes.map((n) =>
        n.id === id ? { ...n, data: { ...n.data, params: { ...n.data.params, [name]: value } } } : n,
      ),
    }))
    api.setParam(id, name, value).catch(() => set({ dirty: true }))
  },

  setPortCount: (id, side, count) => {
    set((s) => ({
      nodes: s.nodes.map((n) => (n.id === id ? { ...n, data: { ...n.data, [side]: count } } : n)),
      dirty: true,
    }))
  },

  setOption: (id, name, value) => {
    set((s) => ({
      nodes: s.nodes.map((n) =>
        n.id === id ? { ...n, data: { ...n.data, options: { ...n.data.options, [name]: value } } } : n,
      ),
      dirty: true,
    }))
  },

  setDomain: (id, domain) => {
    set((s) => {
      const node = s.nodes.find((entry) => entry.id === id)
      if (!node) return s
      const descriptor = s.descriptors[node.data.type]
      if (!domain && descriptor && !descriptor.realtimeSafe) return s

      const nodes = s.nodes.map((entry) =>
        entry.id === id ? { ...entry, data: { ...entry.data, domain } } : entry,
      )
      let domains = s.domains
      if (domain && !domains[domain]) {
        const recommended = descriptor?.recommendedColdBlock ?? 0
        domains = {
          ...domains,
          [domain]: recommended > 0 ? { block: recommended } : {},
        }
      }
      return { nodes, domains: pruneDomains(domains, nodes), dirty: true }
    })
  },

  renameDomain: (domain, next) => {
    set((s) => {
      const name = next.trim()
      if (domainNameIssue(name, s.domains, domain) || !s.domains[domain]) return s
      if (name === domain) return s

      const nodes = s.nodes.map((node) =>
        node.data.domain === domain
          ? { ...node, data: { ...node.data, domain: name } }
          : node,
      )
      const domains = { ...s.domains, [name]: s.domains[domain] }
      delete domains[domain]
      return { nodes, domains, dirty: true }
    })
  },

  setDomainSetting: (domain, key, value) => {
    set((s) => ({
      domains: { ...s.domains, [domain]: { ...s.domains[domain], [key]: value } },
      dirty: true,
    }))
  },

  onNodesChange: (changes) => {
    const structural = changes.some((c) => c.type === 'add' || c.type === 'remove')
    const moved = changes.some((c) => c.type === 'position' && c.dragging === false)
    set((s) => {
      const nodes = applyNodeChanges(changes, s.nodes)
      return {
        nodes,
        domains: structural ? pruneDomains(s.domains, nodes) : s.domains,
        dirty: s.dirty || structural || moved,
      }
    })
  },

  onEdgesChange: (changes) => {
    set((s) => ({
      edges: applyEdgeChanges(changes, s.edges),
      dirty: s.dirty || changes.some((c) => c.type === 'remove'),
    }))
  },

  canConnect: (connection) => {
    const { nodes, descriptors } = get()
    const from = portTypeAt(nodes, descriptors, connection.source, connection.sourceHandle ?? null)
    const to = portTypeAt(nodes, descriptors, connection.target, connection.targetHandle ?? null)

    if (from === undefined || to === undefined) return true
    return from === to
  },

  onConnect: (connection) => {
    if (!get().canConnect(connection)) return
    set((s) => {
      const kept = s.edges.filter(
        (e) => !(e.target === connection.target && e.targetHandle === connection.targetHandle),
      )
      return { edges: addEdge(connection, kept), dirty: true }
    })
  },

  select: (id) => set({ selected: id }),

  setTelemetry: (telemetry) => {
    const before = get().telemetry?.engine
    if (before !== undefined && before !== 'up' && telemetry.engine === 'up') {
      void get().refreshDescriptors()
    }
    set((s) => ({
      telemetry,
      scopes:
        Object.keys(telemetry.scopes).length === 0
          ? s.scopes
          : { ...s.scopes, ...telemetry.scopes },
    }))
  },

  setProfiling: async (on) => {
    await api.setProfiling(on)
  },

  resetStats: async () => {
    await api.resetStats()
  },

  refreshPresets: async () => {
    const { presets, extensionPresets } = await api.presets()
    set({ presets, extensionPresets: extensionPresets ?? [] })
  },

  savePreset: async (name) => {
    await api.savePreset(name)
    await get().refreshPresets()
  },

  loadPreset: async (name) => {
    await api.loadPreset(name)
    await get().load()
  },

  loadExtensionPreset: async (key, name) => {
    await api.loadExtensionPreset(key, name)
    await get().load()
  },

  refreshExtensions: async () => {
    set({ extensions: await api.extensions() })
  },

  selectExtension: (key) => set({ selectedExtension: key }),

  rescanExtensions: async () => {
    set({ extensions: await api.rescanExtensions(), extensionError: null })
  },

  setExtensionEnabled: async (key, enabled) => {
    set({ extensionsBusy: true, extensionError: null })
    try {
      set({ extensions: await api.setExtensionEnabled(key, enabled) })
    } catch (err) {
      set({ extensionError: (err as Error).message })
    } finally {
      set({ extensionsBusy: false })
    }
  },

  saveExtensionSettings: async (key, values) => {
    set({ extensionsBusy: true, extensionError: null })
    try {
      await api.saveExtensionSettings(key, values)
      set({ extensions: await api.extensions() })
    } catch (err) {
      set({ extensionError: (err as Error).message })
    } finally {
      set({ extensionsBusy: false })
    }
  },

  installExtension: async (file) => {
    set({ extensionsBusy: true, extensionError: null })
    try {
      set({ extensions: await api.installExtension(file) })
    } catch (err) {
      set({ extensionError: (err as Error).message })
    } finally {
      set({ extensionsBusy: false })
    }
  },

  deleteExtension: async (key) => {
    set({ extensionsBusy: true, extensionError: null })
    try {
      set({ extensions: await api.deleteExtension(key), selectedExtension: null })
    } catch (err) {
      set({ extensionError: (err as Error).message })
    } finally {
      set({ extensionsBusy: false })
    }
  },

  restartEngine: async () => {
    await api.restartEngine()
  },
}))
