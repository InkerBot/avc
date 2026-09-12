import { useTranslation } from 'react-i18next'
import { useStore } from '../store'

function gainText(gainDb: number): string {
  if (gainDb <= -90) return '−∞'
  return `${gainDb > 0 ? '+' : ''}${gainDb.toFixed(1)}`
}

export function InputMixer({ nodeId }: { nodeId: string }) {
  const edges = useStore((s) => s.edges)
  const setEdgeGain = useStore((s) => s.setEdgeGain)
  const beginHistoryGroup = useStore((s) => s.beginHistoryGroup)
  const endHistoryGroup = useStore((s) => s.endHistoryGroup)
  const { t } = useTranslation()
  const routes = edges.filter((edge) => edge.target === nodeId)

  if (routes.length === 0) {
    return <div className="node-mixer__empty">{t('nodes.mixer.empty')}</div>
  }

  return (
    <div className="node-mixer nodrag nopan">
      <div className="node-mixer__strips">
        {routes.map((edge) => {
          const gainDb = Number(
            (edge.data as { gainDb?: number } | undefined)?.gainDb ?? 0,
          )
          const sourcePort = (edge.sourceHandle ?? 'out:out').slice(4)
          const targetPort = (edge.targetHandle ?? 'in:in').slice(3)
          return (
            <label className="node-mixer__strip" key={edge.id}>
              <span className="node-mixer__source" title={`${edge.source}:${sourcePort}`}>
                {edge.source}
              </span>
              <span className="node-mixer__route">{sourcePort} → {targetPort}</span>
              <input
                className="nodrag"
                type="range"
                min={-90}
                max={24}
                step={0.5}
                value={gainDb}
                aria-label={t('nodes.mixer.routeGainLabel', { node: edge.source })}
                onPointerDown={beginHistoryGroup}
                onPointerUp={endHistoryGroup}
                onChange={(event) => setEdgeGain(edge.id, Number(event.target.value))}
              />
              <output>{gainText(gainDb)} <small>dB</small></output>
              <span className="node-mixer__buttons">
                <button
                  type="button"
                  className="node-mixer__step nodrag"
                  title={t('nodes.mixer.attenuate')}
                  onClick={() => setEdgeGain(edge.id, gainDb - 0.5)}
                >
                  −
                </button>
                <button
                  type="button"
                  className="node-mixer__step nodrag"
                  title={t('nodes.mixer.unityGain')}
                  onClick={() => setEdgeGain(edge.id, 0)}
                >
                  0
                </button>
                <button
                  type="button"
                  className="node-mixer__step nodrag"
                  title={t('nodes.mixer.boost')}
                  onClick={() => setEdgeGain(edge.id, gainDb + 0.5)}
                >
                  +
                </button>
              </span>
            </label>
          )
        })}
      </div>
      <div className="node-mixer__hint">{t('nodes.mixer.applyHint')}</div>
    </div>
  )
}
