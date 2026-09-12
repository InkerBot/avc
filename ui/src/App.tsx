import {
  useCallback,
  useEffect,
  useRef,
  useState,
  type DragEvent as ReactDragEvent,
  type KeyboardEvent as ReactKeyboardEvent,
} from 'react'
import { Trans, useTranslation } from 'react-i18next'
import {
  Background,
  Controls,
  MiniMap,
  Panel,
  ReactFlow,
  ReactFlowProvider,
  useReactFlow,
  type Connection,
  type Edge,
  type NodeTypes,
} from '@xyflow/react'
import '@xyflow/react/dist/style.css'

import { subscribeTelemetry } from './api'
import { extensionTranslationKey, resolveSupportedLanguage, supportedLanguages } from './i18n'
import { useStore } from './store'
import { AvcNode } from './nodes/AvcNode'
import { Palette } from './panel/Palette'
import { AudioDevices } from './panel/AudioDevices'
import { Inspector } from './panel/Inspector'
import { Extensions } from './panel/Extensions'
import { Performance } from './panel/Performance'
import { StatusBar } from './panel/StatusBar'
import { colorOf } from './theme'
import { portTypeAt } from './store'
import { ConfirmDialog } from './components/ConfirmDialog'

const nodeTypes: NodeTypes = { avc: AvcNode }

function panelStartsOpen(query: string) {
  return typeof window === 'undefined'
    || typeof window.matchMedia !== 'function'
    || !window.matchMedia(query).matches
}

function LanguagePicker() {
  const { i18n, t } = useTranslation()
  const language = resolveSupportedLanguage(i18n.resolvedLanguage ?? i18n.language)

  return (
    <select
      className="language-picker"
      value={language}
      aria-label={t('app.language')}
      title={t('app.language')}
      onChange={(event) => void i18n.changeLanguage(event.target.value)}
    >
      {Object.entries(supportedLanguages).map(([code, label]) => (
        <option key={code} value={code}>{label}</option>
      ))}
    </select>
  )
}

function Nav() {
  const view = useStore((s) => s.view)
  const setView = useStore((s) => s.setView)
  const { t } = useTranslation()

  return (
    <div className="segmented segmented--nav">
      <button
        className={`segmented__item${view === 'editor' ? ' segmented__item--on' : ''}`}
        aria-current={view === 'editor' ? 'page' : undefined}
        onClick={() => setView('editor')}
      >
        {t('app.editor')}
      </button>
      <button
        className={`segmented__item${view === 'extensions' ? ' segmented__item--on' : ''}`}
        aria-current={view === 'extensions' ? 'page' : undefined}
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
  const extensions = useStore((s) => s.extensions)
  const savePreset = useStore((s) => s.savePreset)
  const loadPreset = useStore((s) => s.loadPreset)
  const loadExtensionPreset = useStore((s) => s.loadExtensionPreset)
  const dirty = useStore((s) => s.dirty)
  const busy = useStore((s) => s.graphBusy)
  const [name, setName] = useState('')
  const [pending, setPending] = useState<
    | { kind: 'save'; name: string }
    | { kind: 'load'; name: string }
    | { kind: 'loadExtension'; key: string; name: string }
    | null
  >(null)
  const { t } = useTranslation()

  const perform = async (action: NonNullable<typeof pending>) => {
    try {
      if (action.kind === 'save') {
        await savePreset(action.name)
        setName('')
      } else if (action.kind === 'load') {
        await loadPreset(action.name)
      } else {
        await loadExtensionPreset(action.key, action.name)
      }
    } catch {}
  }

  const request = (action: NonNullable<typeof pending>) => {
    const needsConfirmation = action.kind === 'save'
      ? presets.some((preset) => preset.toLocaleLowerCase() === action.name.toLocaleLowerCase())
      : dirty
    if (needsConfirmation) setPending(action)
    else void perform(action)
  }

  return (
    <div className="presets">
      <select
        value=""
        disabled={busy !== null}
        onChange={(e) => {
          if (!e.target.value) return
          const [kind, key, ...rest] = e.target.value.split('\u0000')
          if (kind === 'ext') request({ kind: 'loadExtension', key, name: rest.join('\u0000') })
          else request({ kind: 'load', name: key })
        }}
        aria-label={t('app.loadPreset')}
      >
        <option value="">{t('app.loadPreset')}</option>
        {presets.map((p) => (
          <option key={p} value={`user\u0000${p}`}>
            {p}
          </option>
        ))}
        {extensionPresets.map((p) => {
          const extension = extensions.find((entry) => entry.id === p.extension)
          const extensionName = t(extensionTranslationKey(p.extension, 'name'), {
            defaultValue: extension?.name || p.extension,
          })
          const presetName = t(extensionTranslationKey(p.extension, `presets.${p.name}`), {
            defaultValue: p.name,
          })
          return (
            <option key={`${p.key}/${p.name}`} value={`ext\u0000${p.key}\u0000${p.name}`}>
              {extensionName} · {presetName}
            </option>
          )
        })}
      </select>
      <input
        value={name}
        placeholder={t('app.presetName')}
        onChange={(e) => setName(e.target.value)}
        onKeyDown={(e) => {
          if (e.key === 'Enter' && name.trim() && !busy) {
            request({ kind: 'save', name: name.trim() })
          }
        }}
        aria-label={t('app.presetName')}
      />
      <button
        disabled={!name.trim() || busy !== null}
        onClick={() => request({ kind: 'save', name: name.trim() })}
      >
        {busy === 'savePreset' ? t('app.saving') : t('app.save')}
      </button>
      <ConfirmDialog
        open={pending !== null}
        title={t(pending?.kind === 'save' ? 'app.overwritePresetTitle' : 'app.discardTitle')}
        confirmLabel={t(pending?.kind === 'save' ? 'app.overwrite' : 'app.discard')}
        cancelLabel={t('app.cancel')}
        confirmTone="danger"
        onCancel={() => setPending(null)}
        onConfirm={() => {
          const action = pending
          setPending(null)
          if (action) void perform(action)
        }}
      >
        <p>
          {t(pending?.kind === 'save' ? 'app.overwritePreset' : 'app.discardPresetLoad')}
        </p>
      </ConfirmDialog>
    </div>
  )
}

function Canvas() {
  const {
    nodes,
    edges,
    descriptors,
    onNodesChange,
    onEdgesChange,
    onConnect,
    canConnect,
    addNode,
    select,
    beginHistoryGroup,
    endHistoryGroup,
    graphBusy,
  } = useStore()
  const { screenToFlowPosition } = useReactFlow()
  const [connectionIssue, setConnectionIssue] = useState<string | null>(null)
  const { t } = useTranslation()

  useEffect(() => {
    if (!connectionIssue) return
    const timer = window.setTimeout(() => setConnectionIssue(null), 2600)
    return () => window.clearTimeout(timer)
  }, [connectionIssue])

  const validateConnection = useCallback((connection: Connection | Edge) => {
    const valid = canConnect(connection)
    if (!valid) {
      const from = portTypeAt(nodes, descriptors, connection.source, connection.sourceHandle ?? null)
      const to = portTypeAt(nodes, descriptors, connection.target, connection.targetHandle ?? null)
      const occupied = from === to && edges.some(
        (edge) => edge.target === connection.target
          && edge.targetHandle === connection.targetHandle,
      )
      setConnectionIssue(occupied
        ? t('canvas.inputOccupied')
        : t('canvas.incompatible', { from, to }))
    }
    return valid
  }, [canConnect, descriptors, edges, nodes, t])

  const onDrop = (event: ReactDragEvent) => {
    if (graphBusy) return
    const type = event.dataTransfer.getData('application/x-avc-node')
    if (!type || !descriptors[type]) return
    event.preventDefault()
    addNode(type, screenToFlowPosition({ x: event.clientX, y: event.clientY }))
  }

  return (
    <ReactFlow
      nodes={nodes}
      edges={edges}
      nodesDraggable={graphBusy === null}
      nodesConnectable={graphBusy === null}
      elementsSelectable={graphBusy === null}
      nodeTypes={nodeTypes}
      onNodesChange={onNodesChange}
      onEdgesChange={onEdgesChange}
      onConnect={onConnect}
      isValidConnection={validateConnection}
      onNodeDragStart={beginHistoryGroup}
      onNodeDragStop={endHistoryGroup}
      onDragOver={(event) => {
        if (event.dataTransfer.types.includes('application/x-avc-node')) {
          event.preventDefault()
          event.dataTransfer.dropEffect = 'copy'
        }
      }}
      onDrop={onDrop}
      onNodeClick={(_, node) => select(node.id)}
      onPaneClick={() => select(null)}
      deleteKeyCode={graphBusy ? null : ['Backspace', 'Delete']}
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
      {nodes.length === 0 && (
        <Panel position="top-center" className="canvas__empty">
          {t('canvas.empty')}
        </Panel>
      )}
      {connectionIssue && (
        <Panel position="top-center" className="canvas__feedback" role="status">
          {connectionIssue}
        </Panel>
      )}
    </ReactFlow>
  )
}

function ErrorBanner({ message }: { message: string }) {
  const [copied, setCopied] = useState(false)
  const { t } = useTranslation()

  const copy = async () => {
    try {
      await navigator.clipboard.writeText(message)
    } catch {
      const field = document.createElement('textarea')
      field.value = message
      field.style.position = 'fixed'
      field.style.opacity = '0'
      document.body.appendChild(field)
      field.select()
      document.execCommand('copy')
      field.remove()
    }
    setCopied(true)
    window.setTimeout(() => setCopied(false), 1500)
  }

  return (
    <details className="error-banner">
      <summary className="banner banner--error" title={message}>{message}</summary>
      <div className="error-banner__detail">
        <code>{message}</code>
        <button className="button button--small" onClick={() => void copy()}>
          {t(copied ? 'app.copied' : 'app.copyError')}
        </button>
      </div>
    </details>
  )
}

export default function App() {
  const view = useStore((s) => s.view)
  const load = useStore((s) => s.load)
  const apply = useStore((s) => s.apply)
  const revert = useStore((s) => s.revert)
  const undo = useStore((s) => s.undo)
  const redo = useStore((s) => s.redo)
  const canUndo = useStore((s) => s.history.length > 0)
  const canRedo = useStore((s) => s.future.length > 0)
  const graphBusy = useStore((s) => s.graphBusy)
  const dirty = useStore((s) => s.dirty)
  const error = useStore((s) => s.error)
  const engine = useStore((s) => s.telemetry?.engine)
  const graphError = useStore((s) => s.telemetry?.graphError)
  const setTelemetry = useStore((s) => s.setTelemetry)
  const [ready, setReady] = useState(false)
  const [failure, setFailure] = useState<string | null>(null)
  const [leftPanelOpen, setLeftPanelOpen] = useState(() => panelStartsOpen('(max-width: 850px)'))
  const [rightPanelOpen, setRightPanelOpen] = useState(() => panelStartsOpen('(max-width: 1100px)'))
  const [leftOverlay, setLeftOverlay] = useState(() => !panelStartsOpen('(max-width: 850px)'))
  const [rightOverlay, setRightOverlay] = useState(() => !panelStartsOpen('(max-width: 1100px)'))
  const [rightTab, setRightTab] = useState<'inspector' | 'performance'>('inspector')
  const [confirmRevert, setConfirmRevert] = useState(false)
  const leftToggle = useRef<HTMLButtonElement>(null)
  const rightToggle = useRef<HTMLButtonElement>(null)
  const leftSidebar = useRef<HTMLElement>(null)
  const rightSidebar = useRef<HTMLElement>(null)
  const { t } = useTranslation()

  useEffect(() => {
    load()
      .then(() => setReady(true))
      .catch((e: Error) => setFailure(e.message))
    return subscribeTelemetry(setTelemetry)
  }, [load, setTelemetry])

  useEffect(() => {
    if (typeof window.matchMedia !== 'function') return

    const narrowLeft = window.matchMedia('(max-width: 850px)')
    const narrowRight = window.matchMedia('(max-width: 1100px)')
    const syncLeft = (event: MediaQueryListEvent | MediaQueryList) => {
      setLeftOverlay(event.matches)
      if (event.matches) setLeftPanelOpen(false)
    }
    const syncRight = (event: MediaQueryListEvent | MediaQueryList) => {
      setRightOverlay(event.matches)
      if (event.matches) setRightPanelOpen(false)
    }

    syncLeft(narrowLeft)
    syncRight(narrowRight)
    narrowLeft.addEventListener('change', syncLeft)
    narrowRight.addEventListener('change', syncRight)
    return () => {
      narrowLeft.removeEventListener('change', syncLeft)
      narrowRight.removeEventListener('change', syncRight)
    }
  }, [])

  useEffect(() => {
    if (!dirty) return
    const preventLoss = (event: BeforeUnloadEvent) => {
      event.preventDefault()
      event.returnValue = ''
    }
    window.addEventListener('beforeunload', preventLoss)
    return () => window.removeEventListener('beforeunload', preventLoss)
  }, [dirty])

  useEffect(() => {
    const onKeyDown = (event: KeyboardEvent) => {
      const target = event.target as HTMLElement | null
      // A confirmation owns the keyboard while it is open. Avoid mutating the
      // canvas or closing a drawer behind the dialog.
      if (document.querySelector('.dialog-backdrop')) return
      const activeDrawer = rightOverlay && rightPanelOpen
        ? rightSidebar.current
        : leftOverlay && leftPanelOpen
          ? leftSidebar.current
          : null
      if (event.key === 'Tab' && activeDrawer) {
        const focusable = Array.from(activeDrawer.querySelectorAll<HTMLElement>(
          'button:not(:disabled), input:not(:disabled), select:not(:disabled), textarea:not(:disabled), summary, [tabindex]:not([tabindex="-1"])',
        ))
        if (focusable.length === 0) {
          event.preventDefault()
          activeDrawer.focus()
          return
        }
        const first = focusable[0]
        const last = focusable[focusable.length - 1]
        if (event.shiftKey && (document.activeElement === first || document.activeElement === activeDrawer)) {
          event.preventDefault()
          last.focus()
        } else if (!event.shiftKey && document.activeElement === last) {
          event.preventDefault()
          first.focus()
        }
        return
      }
      if (event.key === 'Escape') {
        if (leftOverlay && leftPanelOpen) {
          setLeftPanelOpen(false)
          leftToggle.current?.focus()
        }
        if (rightOverlay && rightPanelOpen) {
          setRightPanelOpen(false)
          rightToggle.current?.focus()
        }
        return
      }
      if (target?.closest('input, textarea, select, [contenteditable="true"]')) return
      if (graphBusy) return
      if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'z') {
        event.preventDefault()
        if (event.shiftKey) redo()
        else undo()
      } else if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'y') {
        event.preventDefault()
        redo()
      }
    }
    window.addEventListener('keydown', onKeyDown)
    return () => window.removeEventListener('keydown', onKeyDown)
  }, [graphBusy, leftOverlay, leftPanelOpen, redo, rightOverlay, rightPanelOpen, undo])

  useEffect(() => {
    if (leftOverlay && leftPanelOpen) leftSidebar.current?.focus()
  }, [leftOverlay, leftPanelOpen])

  useEffect(() => {
    if (rightOverlay && rightPanelOpen) rightSidebar.current?.focus()
  }, [rightOverlay, rightPanelOpen])

  const onApply = useCallback(() => void apply(), [apply])
  const drawerOpen = leftOverlay && leftPanelOpen || rightOverlay && rightPanelOpen
  const closeDrawers = () => {
    if (leftOverlay && leftPanelOpen) {
      setLeftPanelOpen(false)
      leftToggle.current?.focus()
    }
    if (rightOverlay && rightPanelOpen) {
      setRightPanelOpen(false)
      rightToggle.current?.focus()
    }
  }
  const selectRightTab = (
    event: ReactKeyboardEvent<HTMLButtonElement>,
    tab: 'inspector' | 'performance',
  ) => {
    if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) return
    event.preventDefault()
    const next = event.key === 'ArrowLeft' || event.key === 'Home'
      ? 'inspector'
      : event.key === 'ArrowRight' || event.key === 'End'
        ? 'performance'
        : tab
    setRightTab(next)
    document.getElementById(`${next}-tab`)?.focus()
  }

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
        {view === 'editor' && (
          <>
            <button
              ref={leftToggle}
              className={`button button--small sidebar-toggle${leftPanelOpen ? ' sidebar-toggle--on' : ''}`}
              aria-controls="left-sidebar"
              aria-expanded={leftPanelOpen}
              title={t(leftPanelOpen ? 'app.hideLeftPanel' : 'app.showLeftPanel')}
              onClick={() => {
                const opening = !leftPanelOpen
                setLeftPanelOpen(opening)
                if (opening && leftOverlay && rightOverlay) setRightPanelOpen(false)
              }}
            >
              <span className="sidebar-toggle__icon" aria-hidden="true">◧</span>
              <span className="sidebar-toggle__label">{t('app.leftPanel')}</span>
            </button>
            <button
              ref={rightToggle}
              className={`button button--small sidebar-toggle${rightPanelOpen ? ' sidebar-toggle--on' : ''}`}
              aria-controls="right-sidebar"
              aria-expanded={rightPanelOpen}
              title={t(rightPanelOpen ? 'app.hideRightPanel' : 'app.showRightPanel')}
              onClick={() => {
                const opening = !rightPanelOpen
                setRightPanelOpen(opening)
                if (opening && leftOverlay && rightOverlay) setLeftPanelOpen(false)
              }}
            >
              <span className="sidebar-toggle__icon" aria-hidden="true">◨</span>
              <span className="sidebar-toggle__label">{t('app.rightPanel')}</span>
            </button>
          </>
        )}
        {view === 'editor' && (
          <div className="history-actions" aria-label={t('app.history')}>
            <button
              className="button button--small button--icon"
              disabled={!canUndo || graphBusy !== null}
              aria-label={t('app.undo')}
              title={`${t('app.undo')} (Ctrl+Z)`}
              onClick={undo}
            >
              ↶
            </button>
            <button
              className="button button--small button--icon"
              disabled={!canRedo || graphBusy !== null}
              aria-label={t('app.redo')}
              title={`${t('app.redo')} (Ctrl+Y)`}
              onClick={redo}
            >
              ↷
            </button>
          </div>
        )}
        {view === 'editor' && <Presets />}
        <div className="spacer" />
        <LanguagePicker />
        {view === 'editor' && (
          <>
            <button
              className="button"
              onClick={() => dirty
                ? setConfirmRevert(true)
                : void revert().catch(() => undefined)}
              disabled={graphBusy !== null || !dirty && !error}
            >
              {graphBusy === 'revert' ? t('app.reverting') : t('app.revert')}
            </button>
            <button
              className="button button--primary"
              onClick={onApply}
              disabled={graphBusy !== null || !dirty && !error}
            >
              {graphBusy === 'apply' ? t('app.applying') : t('app.apply')}
            </button>
          </>
        )}
        {(engine !== undefined && engine !== 'up' || graphError || error || dirty && view === 'editor') && (
          <div className="topbar__messages" aria-live="polite">
            {engine !== undefined && engine !== 'up' && (
              <span className="banner">{t(`app.engine.${engine}`)}</span>
            )}
            {graphError && <ErrorBanner message={t('app.graphError', { error: graphError })} />}
            {error && <ErrorBanner message={error} />}
            {dirty && !error && view === 'editor' && <span className="banner">{t('app.dirty')}</span>}
          </div>
        )}
      </header>

      {view === 'extensions' ? (
        <Extensions />
      ) : (
        <ReactFlowProvider>
        <div
          className={`body${drawerOpen ? ' body--drawer-open' : ''}${graphBusy ? ' body--busy' : ''}`}
          aria-busy={graphBusy !== null}
        >
          {drawerOpen && (
            <button
              className="drawer-backdrop"
              aria-label={t('app.closePanel')}
              onClick={closeDrawers}
            />
          )}
          <aside
            ref={leftSidebar}
            id="left-sidebar"
            className={`side side--left${leftPanelOpen ? '' : ' side--closed'}`}
            aria-hidden={!leftPanelOpen}
            role={leftOverlay ? 'dialog' : undefined}
            aria-modal={leftOverlay && leftPanelOpen ? true : undefined}
            aria-label={t('app.leftPanel')}
            tabIndex={leftOverlay && leftPanelOpen ? -1 : undefined}
          >
            <Palette />
            <AudioDevices />
          </aside>
          <main className="canvas">
            <Canvas />
          </main>
          <aside
            ref={rightSidebar}
            id="right-sidebar"
            className={`side side--right${rightPanelOpen ? '' : ' side--closed'}`}
            aria-hidden={!rightPanelOpen}
            role={rightOverlay ? 'dialog' : undefined}
            aria-modal={rightOverlay && rightPanelOpen ? true : undefined}
            aria-label={t('app.rightPanel')}
            tabIndex={rightOverlay && rightPanelOpen ? -1 : undefined}
          >
            <div className="side__tabs" role="tablist" aria-label={t('app.rightPanel')}>
              <button
                id="inspector-tab"
                role="tab"
                aria-controls="right-tabpanel"
                aria-selected={rightTab === 'inspector'}
                tabIndex={rightTab === 'inspector' ? 0 : -1}
                className={rightTab === 'inspector' ? 'side__tab side__tab--on' : 'side__tab'}
                onClick={() => setRightTab('inspector')}
                onKeyDown={(event) => selectRightTab(event, 'inspector')}
              >
                {t('inspector.title')}
              </button>
              <button
                id="performance-tab"
                role="tab"
                aria-controls="right-tabpanel"
                aria-selected={rightTab === 'performance'}
                tabIndex={rightTab === 'performance' ? 0 : -1}
                className={rightTab === 'performance' ? 'side__tab side__tab--on' : 'side__tab'}
                onClick={() => setRightTab('performance')}
                onKeyDown={(event) => selectRightTab(event, 'performance')}
              >
                {t('perf.title')}
              </button>
            </div>
            <div
              id="right-tabpanel"
              role="tabpanel"
              aria-labelledby={`${rightTab}-tab`}
            >
              {rightTab === 'inspector' ? <Inspector /> : <Performance />}
            </div>
          </aside>
        </div>
        </ReactFlowProvider>
      )}

      <StatusBar />
      <ConfirmDialog
        open={confirmRevert}
        title={t('app.discardTitle')}
        confirmLabel={t('app.discard')}
        cancelLabel={t('app.cancel')}
        confirmTone="danger"
        onCancel={() => setConfirmRevert(false)}
        onConfirm={() => {
          setConfirmRevert(false)
          void revert().catch(() => undefined)
        }}
      >
        <p>{t('app.discardRevert')}</p>
      </ConfirmDialog>
    </div>
  )
}
