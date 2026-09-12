import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import { useStore } from '../store'
import type { DomainTelemetry } from '../api'

function Row({ label, value, warn, hint }: {
  label: string
  value: string
  warn?: boolean
  hint?: string
}) {
  return (
    <div className={`perf__row${warn ? ' perf__row--warn' : ''}`}>
      <span className="perf__label" title={hint}>{label}</span>
      <span className="perf__value">{value}</span>
    </div>
  )
}

function LatencyBar({ io, graph }: { io: number; graph: number }) {
  const { t } = useTranslation()
  const total = io + graph
  if (total <= 0) return null

  const graphShare = (graph / total) * 100
  return (
    <div className="perf__bar" title={t('perf.breakdown', {
      io: io.toFixed(2),
      graph: graph.toFixed(2),
    })}>
      <div className="perf__bar-io" style={{ width: `${100 - graphShare}%` }} />
      <div className="perf__bar-graph" style={{ width: `${graphShare}%` }} />
    </div>
  )
}

function NodeCosts({ costs, budgetUs }: { costs: Record<string, number>; budgetUs: number }) {
  const { t } = useTranslation()
  const rows = Object.entries(costs).sort((a, b) => b[1] - a[1])
  if (rows.length === 0) {
    return <p className="hint">{t('perf.profilingHint')}</p>
  }

  const worst = Math.max(rows[0][1], 1e-3)
  return (
    <div className="perf__costs">
      {rows.map(([id, us]) => (
        <div className="perf__cost" key={id}>
          <span className="perf__cost-name" title={id}>{id}</span>
          <span className="perf__cost-bar">
            <span className="perf__cost-fill" style={{ width: `${(us / worst) * 100}%` }} />
          </span>
          <span className="perf__cost-us">
            {us.toFixed(1)} µs
            {budgetUs > 0 && ` · ${((us / budgetUs) * 100).toFixed(1)}%`}
          </span>
        </div>
      ))}
    </div>
  )
}

function ColdDomains({ domains }: { domains: DomainTelemetry[] }) {
  const { t } = useTranslation()
  const cold = domains.filter((d) => d.cold)
  if (cold.length === 0) return null

  return (
    <>
      <div className="perf__gap" />
      <h3 className="perf__heading">{t('perf.cold')}</h3>
      {cold.map((domain) => (
        <div className="perf__domain" key={domain.name}>
          <Row
            label={t('perf.coldBlock', { name: domain.name })}
            value={t('perf.coldBlockValue', {
              frames: domain.block,
              ms: domain.blockMs.toFixed(1),
            })}
          />
          <Row
            label={t('perf.coldAdded')}
            value={`${domain.latencyMs.toFixed(1)} ms`}
          />
          <Row
            label={t('perf.coldLoad')}
            value={`${domain.dspUsAvg.toFixed(0)} µs · ${domain.loadAvg.toFixed(1)}%`}
            warn={domain.loadAvg > 70}
          />
          <Row
            label={t('perf.coldMisses')}
            value={
              domain.underruns === 0 && domain.overruns === 0
                ? t('perf.coldClean')
                : t('perf.coldMissValue', {
                    under: domain.underruns,
                    over: domain.overruns,
                  })
            }
            warn={domain.underruns > 0 || domain.overruns > 0}
            hint={t('perf.coldMissesHint')}
          />
        </div>
      ))}
    </>
  )
}

export function Performance() {
  const tel = useStore((s) => s.telemetry)
  const setProfiling = useStore((s) => s.setProfiling)
  const resetStats = useStore((s) => s.resetStats)
  const [pending, setPending] = useState<'profiling' | 'reset' | null>(null)
  const [error, setError] = useState<string | null>(null)
  const { t } = useTranslation()

  const updateProfiling = async (enabled: boolean) => {
    setPending('profiling')
    setError(null)
    try {
      await setProfiling(enabled)
    } catch (cause) {
      setError((cause as Error).message)
    } finally {
      setPending(null)
    }
  }

  const reset = async () => {
    setPending('reset')
    setError(null)
    try {
      await resetStats()
    } catch (cause) {
      setError((cause as Error).message)
    } finally {
      setPending(null)
    }
  }

  if (!tel) {
    return (
      <div className="panel">
        <h2 className="panel__title">{t('perf.title')}</h2>
        <p className="hint">{t('status.connecting')}</p>
      </div>
    )
  }

  const budgetUs = tel.blockMs * 1000

  return (
    <div className="panel">
      <h2 className="panel__title">{t('perf.title')}</h2>

      <Row
        label={t('perf.total')}
        value={`${tel.totalLatencyMs.toFixed(2)} ms`}
        warn={tel.totalLatencyMs > 20}
      />
      <LatencyBar io={tel.ioLatencyMs} graph={tel.graphLatencyMs} />
      <Row label={t('perf.io')} value={`${tel.ioLatencyMs.toFixed(2)} ms`} />
      <Row
        label={t('perf.graph')}
        value={
          tel.graphLatencyFrames === 0
            ? t('perf.none')
            : t('perf.graphValue', {
                ms: tel.graphLatencyMs.toFixed(2),
                frames: tel.graphLatencyFrames,
              })
        }
        hint={t('perf.graphHint')}
      />

      <ColdDomains domains={tel.domains ?? []} />

      {Object.keys(tel.outputLatencyMs ?? {}).length > 1 &&
        Object.entries(tel.outputLatencyMs).map(([node, ms]) => (
          <Row
            key={node}
            label={t('perf.output', { node })}
            value={`${ms.toFixed(2)} ms`}
          />
        ))}

      <div className="perf__gap" />

      <Row
        label={t('perf.dspAvg')}
        value={`${tel.dspUsAvg.toFixed(1)} µs · ${tel.loadAvg.toFixed(1)}%`}
        warn={tel.loadAvg > 50}
      />
      <Row
        label={t('perf.dspPeak')}
        value={`${tel.dspUs.toFixed(1)} µs · ${tel.load.toFixed(1)}%`}
        warn={tel.load > 80}
        hint={t('perf.dspPeakHint')}
      />
      <Row
        label={t('perf.jitter')}
        value={`${tel.jitterUs.toFixed(1)} µs`}
        warn={tel.jitterUs > budgetUs * 0.5}
        hint={t('perf.jitterHint')}
      />
      <Row label={t('status.xruns')} value={String(tel.xruns)} warn={tel.xruns > 0} />

      <div className="perf__actions">
        <label className="field--check">
          <input
            type="checkbox"
            checked={tel.profiling}
            disabled={pending !== null}
            onChange={(e) => void updateProfiling(e.target.checked)}
          />
          <span>{t('perf.profiling')}</span>
        </label>
        <button className="button button--small" disabled={pending !== null} onClick={() => void reset()}>
          {pending === 'reset' ? t('perf.resetting') : t('perf.reset')}
        </button>
      </div>
      {error && <p className="field__error">{error}</p>}

      {tel.profiling && <NodeCosts costs={tel.nodeCost} budgetUs={budgetUs} />}
    </div>
  )
}
