import { useCallback, useEffect, useState } from 'react'
import { Trans, useTranslation } from 'react-i18next'
import {
  Background,
  Controls,
  MiniMap,
  ReactFlow,
  ReactFlowProvider,
  type NodeTypes,
} from '@xyflow/react'
import '@xyflow/react/dist/style.css'

import { subscribeTelemetry } from './api'
import { useStore } from './store'
import { AvcNode } from './nodes/AvcNode'
import { Palette } from './panel/Palette'
import { AudioDevices } from './panel/AudioDevices'
import { Inspector } from './panel/Inspector'
import { Extensions } from './panel/Extensions'
import { Performance } from './panel/Performance'
import { StatusBar } from './panel/StatusBar'
import { colorOf } from './theme'

const nodeTypes: NodeTypes = { avc: AvcNode }

function Nav() {
  const view = useStore((s) => s.view)
  const setView = useStore((s) => s.setView)
  const { t } = useTranslation()

  return (
    <div className="segmented segmented--nav">
      <button
        className={`segmented__item${view === 'editor' ? ' segmented__item--on' : ''}`}
        onClick={() => setView('editor')}
      >
        {t('app.editor')}
      </button>
      <button
        className={`segmented__item${view === 'extensions' ? ' segmented__item--on' : ''}`}
        onClick={() => setView('extensions')}
      >
        {t('app.extensions')}
      </button>
    </div>
  )
}

function Presets() {
  const presets = useStore((s) => s.presets)
  const extensionPresets = useStore((s) => s.extensionPresets)
  const savePreset = useStore((s) => s.savePreset)
  const loadPreset = useStore((s) => s.loadPreset)
  const loadExtensionPreset = useStore((s) => s.loadExtensionPreset)
  const [name, setName] = useState('')
  const { t } = useTranslation()

  return (
    <div className="presets">
      <select
        value=""
        onChange={(e) => {
          if (!e.target.value) return
          const [kind, key, ...rest] = e.target.value.split('\u0000')
          if (kind === 'ext') void loadExtensionPreset(key, rest.join('\u0000'))
          else void loadPreset(key)
        }}
        aria-label={t('app.loadPreset')}
      >
        <option value="">{t('app.loadPreset')}</option>
        {presets.map((p) => (
          <option key={p} value={`user\u0000${p}`}>
            {p}
          </option>
        ))}
        {extensionPresets.map((p) => (
          <option key={`${p.key}/${p.name}`} value={`ext\u0000${p.key}\u0000${p.name}`}>
            {p.extension} · {p.name}
          </option>
        ))}
      </select>
      <input
        value={name}
        placeholder={t('app.presetName')}
        onChange={(e) => setName(e.target.value)}
        aria-label={t('app.presetName')}
      />
      <button
        disabled={!name.trim()}
        onClick={() => {
          void savePreset(name.trim())
          setName('')
        }}
      >
        {t('app.save')}
      </button>
    </div>
  )
}

function Canvas() {
  const { nodes, edges, onNodesChange, onEdgesChange, onConnect, canConnect, select } = useStore()

  return (
    <ReactFlow
      nodes={nodes}
      edges={edges}
      nodeTypes={nodeTypes}
      onNodesChange={onNodesChange}
      onEdgesChange={onEdgesChange}
      onConnect={onConnect}
      isValidConnection={canConnect}
      onNodeClick={(_, node) => select(node.id)}
      onPaneClick={() => select(null)}
      deleteKeyCode={['Backspace', 'Delete']}
      fitView
      proOptions={{ hideAttribution: true }}
      defaultEdgeOptions={{ animated: false, style: { strokeWidth: 2 } }}
    >
      <Background gap={22} size={1} color="#2b2730" />
      <MiniMap
        pannable
        zoomable
        nodeColor={(n) => {
          const store = useStore.getState()
          const descriptor = store.descriptors[(n.data as { type: string }).type]
          return descriptor ? colorOf(descriptor.category) : '#666'
        }}
        maskColor="rgba(12,10,14,0.7)"
      />
      <Controls showInteractive={false} />
    </ReactFlow>
  )
}

export default function App() {
  const view = useStore((s) => s.view)
  const load = useStore((s) => s.load)
  const apply = useStore((s) => s.apply)
  const revert = useStore((s) => s.revert)
  const dirty = useStore((s) => s.dirty)
  const error = useStore((s) => s.error)
  const engine = useStore((s) => s.telemetry?.engine)
  const graphError = useStore((s) => s.telemetry?.graphError)
  const setTelemetry = useStore((s) => s.setTelemetry)
  const [ready, setReady] = useState(false)
  const [failure, setFailure] = useState<string | null>(null)
  const { t } = useTranslation()

  useEffect(() => {
    load()
      .then(() => setReady(true))
      .catch((e: Error) => setFailure(e.message))
    return subscribeTelemetry(setTelemetry)
  }, [load, setTelemetry])

  const onApply = useCallback(() => void apply(), [apply])

  if (failure) {
    return (
      <div className="boot boot--error">
        <h1>{t('app.bootTitle')}</h1>
        <p>{failure}</p>
        <p className="hint">
          <Trans i18nKey="app.bootHint" components={{ code: <code /> }} />
        </p>
      </div>
    )
  }
  if (!ready) return <div className="boot">{t('app.loading')}</div>

  return (
    <div className="app">
      <header className="topbar">
        <span className="brand">AVC</span>
        <Nav />
        {view === 'editor' && <Presets />}
        <div className="spacer" />
        {engine !== undefined && engine !== 'up' && (
          <span className="banner">{t(`app.engine.${engine}`)}</span>
        )}
        {graphError && <span className="banner banner--error">{t('app.graphError', { error: graphError })}</span>}
        {error && <span className="banner banner--error">{error}</span>}
        {dirty && !error && view === 'editor' && <span className="banner">{t('app.dirty')}</span>}
        {view === 'editor' && (
          <>
            <button className="button" onClick={() => void revert()} disabled={!dirty && !error}>
              {t('app.revert')}
            </button>
            <button
              className="button button--primary"
              onClick={onApply}
              disabled={!dirty && !error}
            >
              {t('app.apply')}
            </button>
          </>
        )}
      </header>

      {view === 'extensions' ? (
        <Extensions />
      ) : (
        <div className="body">
          <aside className="side side--left">
            <Palette />
            <AudioDevices />
          </aside>
          <main className="canvas">
            <ReactFlowProvider>
              <Canvas />
            </ReactFlowProvider>
          </main>
          <aside className="side side--right">
            <Inspector />
            <Performance />
          </aside>
        </div>
      )}

      <StatusBar />
    </div>
  )
}
