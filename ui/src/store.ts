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
  type TextTelemetry,
} from './api'
import { extensionRuntime } from './extensions/runtime'

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

function validEdges(
  nodes: AvcNode[],
  descriptors: Record<string, NodeDescriptor>,
  edges: Edge[],
): Edge[] {
  const byId = new Map(nodes.map((node) => [node.id, node]))

  return edges.filter((edge) => {
    const source = byId.get(edge.source)
    const target = byId.get(edge.target)
    if (!source || !target) return false

    const sourceDescriptor = descriptors[source.data.type]
    const targetDescriptor = descriptors[target.data.type]
    const sourceHandle = edge.sourceHandle ?? null
    const targetHandle = edge.targetHandle ?? null

    // Keep edges for unavailable extension nodes intact. Once a descriptor is
    // known, however, a missing handle means the edge points at a removed port.
    const sourceType = sourceDescriptor
      ? portTypeAt(nodes, descriptors, edge.source, sourceHandle)
      : undefined
    const targetType = targetDescriptor
      ? portTypeAt(nodes, descriptors, edge.target, targetHandle)
      : undefined
    if (sourceDescriptor && (!sourceHandle?.startsWith('out:') || sourceType === undefined)) {
      return false
    }
    if (targetDescriptor && (!targetHandle?.startsWith('in:') || targetType === undefined)) {
      return false
    }

    return sourceType === undefined || targetType === undefined || sourceType === targetType
  })
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
      width: n.ui?.width,
      height: n.ui?.height,
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
    data: { gainDb: e.gain_db ?? 0 },
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

    const ui: SpecNode['ui'] = {
      x: Math.round(n.position.x),
      y: Math.round(n.position.y),
    }
    if (n.width !== undefined && n.width > 0) ui.width = Math.round(n.width)
    if (n.height !== undefined && n.height > 0) ui.height = Math.round(n.height)

    const node: SpecNode = {
      id: n.id,
      type: n.data.type,
      params,
      ui,
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
    edges: edges.map((e) => {
      const edge = {
        from: { node: e.source, port: (e.sourceHandle ?? 'out:out').slice(4) },
        to: { node: e.target, port: (e.targetHandle ?? 'in:in').slice(3) },
      } as GraphSpec['edges'][number]
      const gainDb = Number((e.data as { gainDb?: number } | undefined)?.gainDb ?? 0)
      if (gainDb !== 0) edge.gain_db = gainDb
      return edge
    }),
  }
}

export type View = 'editor' | 'extensions'

export interface TextMessage {
  id: string
  text: string
  revision: number
  final: boolean
}

export interface TextConversation {
  history: TextMessage[]
  draft: TextMessage | null
}

const MAX_TEXT_HISTORY = 200

function upsertHistory(history: TextMessage[], message: TextMessage): TextMessage[] {
  const at = history.findIndex((entry) => entry.id === message.id)
  const next = at < 0
    ? [...history, message]
    : history.map((entry, index) => index === at ? message : entry)
  return next.length > MAX_TEXT_HISTORY ? next.slice(-MAX_TEXT_HISTORY) : next
}

export function updateTextConversation(
  previous: TextConversation | undefined,
  update: TextTelemetry | string,
): TextConversation {
  const conversation = previous ?? { history: [], draft: null }
  if (typeof update === 'string' || !update.stream || update.segment <= 0) {
    const text = typeof update === 'string' ? update : update.text
    return {
      history: conversation.history,
      draft: text ? { id: 'plain', text, revision: 0, final: false } : null,
    }
  }

  const id = `${update.stream}:${update.segment}`
  const message: TextMessage = {
    id,
    text: update.text,
    revision: update.revision,
    final: update.final,
  }
  const completed = conversation.history.find((entry) => entry.id === id)
  if (completed && completed.revision > message.revision) return conversation

  let history = conversation.history
  if (conversation.draft && conversation.draft.id !== id) {
    history = upsertHistory(history, { ...conversation.draft, final: false })
  }

  if (message.final) {
    if (message.text) history = upsertHistory(history, message)
    return {
      history,
      draft: conversation.draft?.id === id ? null : conversation.draft,
    }
  }

  // Do not let a delayed partial frame turn an already-final message back into a draft.
  if (completed?.final && completed.revision >= message.revision) {
    return { history, draft: conversation.draft?.id === id ? null : conversation.draft }
  }
  return { history, draft: message }
}

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
  graphBusy: 'apply' | 'revert' | 'savePreset' | 'loadPreset' | null
  history: GraphSnapshot[]
  future: GraphSnapshot[]
  historyAnchor: GraphSnapshot | null
  savedFingerprint: string
  telemetry: Telemetry | null

  scopes: Record<string, { wave: number[]; bands: number[] }>
  texts: Record<string, TextConversation>
  selected: string | null

  setView: (view: View) => void
  load: () => Promise<void>
  refreshAudioDevices: () => Promise<void>
  refreshDescriptors: () => Promise<void>
  apply: () => Promise<void>
  revert: () => Promise<void>
  undo: () => void
  redo: () => void
  beginHistoryGroup: () => void
  endHistoryGroup: () => void
  addNode: (type: string, at: { x: number; y: number }) => void
  setParam: (id: string, name: string, value: number) => void
  setEdgeGain: (id: string, gainDb: number) => void
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
  clearTextHistory: (id: string) => void
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
  chooseExtension: () => Promise<void>
  deleteExtension: (key: string) => Promise<void>
  restartEngine: () => Promise<void>
  forceRestartEngine: () => Promise<void>
}

interface GraphSnapshot {
  nodes: AvcNode[]
  edges: Edge[]
  domains: Record<string, SpecDomain>
}

const HISTORY_LIMIT = 50

function cloneSnapshot(source: Pick<State, 'nodes' | 'edges' | 'domains'>): GraphSnapshot {
  return {
    nodes: source.nodes.map((node) => ({
      ...node,
      position: { ...node.position },
      data: {
        ...node.data,
        params: { ...node.data.params },
        options: { ...node.data.options },
      },
    })),
    edges: source.edges.map((edge) => ({
      ...edge,
      data: edge.data ? { ...edge.data } : undefined,
    })),
    domains: Object.fromEntries(
      Object.entries(source.domains).map(([name, domain]) => [name, { ...domain }]),
    ),
  }
}

function fingerprint(source: Pick<State, 'nodes' | 'edges' | 'domains'>): string {
  return JSON.stringify(flowToSpec(source.nodes, source.edges, 1, source.domains))
}

function remember(source: State) {
  if (source.historyAnchor) return {}
  return {
    history: [...source.history, cloneSnapshot(source)].slice(-HISTORY_LIMIT),
    future: [],
  }
}

function syncLiveParams(from: AvcNode[], to: AvcNode[], markFailed: () => void) {
  const previous = new Map(from.map((node) => [node.id, node]))
  to.forEach((node) => {
    const before = previous.get(node.id)
    if (!before) return
    Object.entries(node.data.params).forEach(([name, value]) => {
      if (before.data.params[name] !== value) {
        api.setParam(node.id, name, value).catch(markFailed)
      }
    })
  })
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
  graphBusy: null,
  history: [],
  future: [],
  historyAnchor: null,
  savedFingerprint: '',
  telemetry: null,
  scopes: {},
  texts: {},
  selected: null,

  setView: (view) => set({ view }),

  refreshAudioDevices: async () => {
    set({ audioDevices: await api.audioDevices() })
  },

  refreshDescriptors: async () => {
    const descriptors = await api.descriptors()
    const byType: Record<string, NodeDescriptor> = {}
    descriptors.forEach((d) => (byType[d.type] = d))
    set((current) => {
      const edges = validEdges(current.nodes, byType, current.edges)
      return {
        descriptors: byType,
        palette: descriptors,
        edges,
        dirty: current.dirty || edges.length !== current.edges.length,
      }
    })
    void get().refreshExtensions()
    void get().refreshPresets()
  },

  load: async () => {
    const [descriptors, graph, audioDevices, extensions] = await Promise.all([
      api.descriptors(),
      api.graph(),
      api.audioDevices(),
      api.extensions(),
    ])
    void extensionRuntime.sync(extensions)
    const byType: Record<string, NodeDescriptor> = {}
    descriptors.forEach((d) => (byType[d.type] = d))
    const { nodes, edges: specEdges } = specToFlow(graph.spec)
    const edges = validEdges(nodes, byType, specEdges)
    const domains = graph.spec.domains ?? {}
    set({
      descriptors: byType,
      palette: descriptors,
      audioDevices,
      extensions,
      nodes,
      edges,
      domains,
      dirty: false,
      error: null,
      history: [],
      future: [],
      historyAnchor: null,
      savedFingerprint: fingerprint({ nodes, edges, domains }),
      scopes: {},
      texts: {},
    })
    void get().refreshPresets()
  },

  apply: async () => {
    const current = get()
    const { nodes, domains } = current
    const edges = validEdges(nodes, current.descriptors, current.edges)
    set({ graphBusy: 'apply', error: null, edges })
    try {
      const spec = flowToSpec(nodes, edges, 1, domains)
      await api.applyGraph(spec)
      const savedFingerprint = fingerprint({ nodes, edges, domains })
      set((current) => ({
        dirty: fingerprint(current) !== savedFingerprint,
        error: null,
        savedFingerprint,
      }))
    } catch (err) {
      set({ error: (err as Error).message })
    } finally {
      set({ graphBusy: null })
    }
  },

  revert: async () => {
    set({ graphBusy: 'revert', error: null })
    try {
      await get().load()
    } catch (err) {
      set({ error: (err as Error).message })
      throw err
    } finally {
      set({ graphBusy: null })
    }
  },

  undo: () => {
    const current = get()
    const target = current.history.at(-1)
    if (!target) return
    const restored = cloneSnapshot(target)
    set({
      ...restored,
      history: current.history.slice(0, -1),
      future: [cloneSnapshot(current), ...current.future].slice(0, HISTORY_LIMIT),
      historyAnchor: null,
      selected: null,
      dirty: fingerprint(restored) !== current.savedFingerprint,
    })
    syncLiveParams(current.nodes, restored.nodes, () => set({ dirty: true }))
  },

  redo: () => {
    const current = get()
    const target = current.future[0]
    if (!target) return
    const restored = cloneSnapshot(target)
    set({
      ...restored,
      history: [...current.history, cloneSnapshot(current)].slice(-HISTORY_LIMIT),
      future: current.future.slice(1),
      historyAnchor: null,
      selected: null,
      dirty: fingerprint(restored) !== current.savedFingerprint,
    })
    syncLiveParams(current.nodes, restored.nodes, () => set({ dirty: true }))
  },

  beginHistoryGroup: () => set((s) => (
    s.historyAnchor ? {} : { historyAnchor: cloneSnapshot(s) }
  )),

  endHistoryGroup: () => set((s) => {
    const anchor = s.historyAnchor
    if (!anchor) return {}
    if (fingerprint(anchor) === fingerprint(s)) return { historyAnchor: null }
    return {
      history: [...s.history, anchor].slice(-HISTORY_LIMIT),
      future: [],
      historyAnchor: null,
      dirty: fingerprint(s) !== s.savedFingerprint,
    }
  }),

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
      return {
        ...remember(s),
        nodes: [...s.nodes, node],
        domains,
        selected: id,
        dirty: true,
      }
    })
  },

  setParam: (id, name, value) => {
    set((s) => {
      const nodes = s.nodes.map((n) =>
        n.id === id ? { ...n, data: { ...n.data, params: { ...n.data.params, [name]: value } } } : n,
      )
      return {
        nodes,
        savedFingerprint: s.dirty
          ? s.savedFingerprint
          : fingerprint({ nodes, edges: s.edges, domains: s.domains }),
      }
    })
    api.setParam(id, name, value).catch(() => set({ dirty: true }))
  },

  setEdgeGain: (id, gainDb) => {
    set((s) => ({
      ...remember(s),
      edges: s.edges.map((edge) =>
        edge.id === id
          ? { ...edge, data: { ...edge.data, gainDb: Math.max(-90, Math.min(24, gainDb)) } }
          : edge,
      ),
      dirty: true,
    }))
  },

  setPortCount: (id, side, count) => {
    set((s) => {
      const node = s.nodes.find((entry) => entry.id === id)
      const descriptor = node ? s.descriptors[node.data.type] : undefined
      const dynamic = side === 'inputs' ? descriptor?.dynamicInputs : descriptor?.dynamicOutputs
      if (!node || !dynamic) return s

      const nextCount = Math.max(1, Math.trunc(count))
      const nodes = s.nodes.map((entry) =>
        entry.id === id
          ? { ...entry, data: { ...entry.data, [side]: nextCount } }
          : entry,
      )
      return {
        ...remember(s),
        nodes,
        edges: validEdges(nodes, s.descriptors, s.edges),
        dirty: true,
      }
    })
  },

  setOption: (id, name, value) => {
    set((s) => ({
      ...remember(s),
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
      return {
        ...remember(s),
        nodes,
        domains: pruneDomains(domains, nodes),
        dirty: true,
      }
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
      return { ...remember(s), nodes, domains, dirty: true }
    })
  },

  setDomainSetting: (domain, key, value) => {
    set((s) => ({
      ...remember(s),
      domains: { ...s.domains, [domain]: { ...s.domains[domain], [key]: value } },
      dirty: true,
    }))
  },

  onNodesChange: (changes) => {
    const structural = changes.some((c) => c.type === 'add' || c.type === 'remove')
    const moved = changes.some((c) => c.type === 'position' && c.dragging === false)
    const resized = changes.some((c) => c.type === 'dimensions' && c.resizing === false)
    set((s) => {
      const nodes = applyNodeChanges(changes, s.nodes)
      const changed = structural || moved || resized
      return {
        ...(changed ? remember(s) : {}),
        nodes,
        edges: structural ? validEdges(nodes, s.descriptors, s.edges) : s.edges,
        domains: structural ? pruneDomains(s.domains, nodes) : s.domains,
        dirty: s.dirty || changed,
      }
    })
  },

  onEdgesChange: (changes) => {
    set((s) => {
      const edges = applyEdgeChanges(changes, s.edges)
      const structural = changes.some((c) => c.type === 'add' || c.type === 'remove')
      const changed = structural && (
        edges.length !== s.edges.length
        || edges.some((edge, index) => edge.id !== s.edges[index]?.id)
      )
      return {
        ...(changed ? remember(s) : {}),
        edges,
        dirty: s.dirty || changed,
      }
    })
  },

  canConnect: (connection) => {
    const { nodes, descriptors, edges } = get()
    const from = portTypeAt(nodes, descriptors, connection.source, connection.sourceHandle ?? null)
    const to = portTypeAt(nodes, descriptors, connection.target, connection.targetHandle ?? null)

    if (from === undefined || to === undefined) return false
    if (from !== to) return false
    if (to === AUDIO_PORT_TYPE) return true
    return !edges.some(
      (edge) => edge.target === connection.target && edge.targetHandle === connection.targetHandle,
    )
  },

  onConnect: (connection) => {
    if (!get().canConnect(connection)) return
    set((s) => ({
      ...remember(s),
      edges: addEdge({ ...connection, data: { gainDb: 0 } }, s.edges),
      dirty: true,
    }))
  },

  select: (id) => set({ selected: id }),

  clearTextHistory: (id) => set((s) => {
    const conversation = s.texts[id]
    if (!conversation || conversation.history.length === 0) return s
    return {
      texts: {
        ...s.texts,
        [id]: { history: [], draft: conversation.draft },
      },
    }
  }),

  setTelemetry: (telemetry) => {
    const before = get().telemetry?.engine
    const scopes = telemetry.scopes ?? {}
    const texts = telemetry.texts ?? {}
    if (before !== undefined && before !== 'up' && telemetry.engine === 'up') {
      void get().refreshDescriptors()
    }
    set((s) => {
      let conversations = s.texts
      for (const [id, update] of Object.entries(texts)) {
        if (conversations === s.texts) conversations = { ...s.texts }
        conversations[id] = updateTextConversation(conversations[id], update)
      }
      return {
        telemetry,
        scopes:
          Object.keys(scopes).length === 0
            ? s.scopes
            : { ...s.scopes, ...scopes },
        texts: conversations,
      }
    })
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
    set({ graphBusy: 'savePreset', error: null })
    try {
      const current = get()
      const { nodes, domains } = current
      const edges = validEdges(nodes, current.descriptors, current.edges)
      if (edges.length !== current.edges.length) set({ edges, dirty: true })
      await api.savePreset(name, flowToSpec(nodes, edges, 1, domains))
      await get().refreshPresets()
    } catch (err) {
      set({ error: (err as Error).message })
      throw err
    } finally {
      set({ graphBusy: null })
    }
  },

  loadPreset: async (name) => {
    set({ graphBusy: 'loadPreset', error: null })
    try {
      await api.loadPreset(name)
      await get().load()
    } catch (err) {
      set({ error: (err as Error).message })
      throw err
    } finally {
      set({ graphBusy: null })
    }
  },

  loadExtensionPreset: async (key, name) => {
    set({ graphBusy: 'loadPreset', error: null })
    try {
      await api.loadExtensionPreset(key, name)
      await get().load()
    } catch (err) {
      set({ error: (err as Error).message })
      throw err
    } finally {
      set({ graphBusy: null })
    }
  },

  refreshExtensions: async () => {
    const extensions = await api.extensions()
    void extensionRuntime.sync(extensions)
    set({ extensions })
  },

  selectExtension: (key) => set({ selectedExtension: key }),

  rescanExtensions: async () => {
    set({ extensionsBusy: true, extensionError: null })
    try {
      const extensions = await api.rescanExtensions()
      void extensionRuntime.sync(extensions)
      set({ extensions })
    } catch (err) {
      set({ extensionError: (err as Error).message })
    } finally {
      set({ extensionsBusy: false })
    }
  },

  setExtensionEnabled: async (key, enabled) => {
    set({ extensionsBusy: true, extensionError: null })
    try {
      const extensions = await api.setExtensionEnabled(key, enabled)
      void extensionRuntime.sync(extensions)
      set({ extensions })
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
      const extensions = await api.extensions()
      void extensionRuntime.sync(extensions)
      set({ extensions })
    } catch (err) {
      set({ extensionError: (err as Error).message })
    } finally {
      set({ extensionsBusy: false })
    }
  },

  chooseExtension: async () => {
    set({ extensionsBusy: true, extensionError: null })
    try {
      const extensions = await api.chooseExtension()
      if (extensions) {
        void extensionRuntime.sync(extensions)
        set({ extensions })
      }
    } catch (err) {
      set({ extensionError: (err as Error).message })
    } finally {
      set({ extensionsBusy: false })
    }
  },

  deleteExtension: async (key) => {
    set({ extensionsBusy: true, extensionError: null })
    try {
      const extensions = await api.deleteExtension(key)
      void extensionRuntime.sync(extensions)
      set({ extensions, selectedExtension: null })
    } catch (err) {
      set({ extensionError: (err as Error).message })
    } finally {
      set({ extensionsBusy: false })
    }
  },

  restartEngine: async () => {
    await api.restartEngine()
  },

  forceRestartEngine: async () => {
    await api.forceRestartEngine()
  },
}))
