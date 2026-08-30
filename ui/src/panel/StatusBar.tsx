import { useState } from 'react'
import { useStore } from '../store'
import { useTranslation } from 'react-i18next'

function Stat({ label, value, warn }: { label: string; value: string; warn?: boolean }) {
  return (
    <div className={`stat${warn ? ' stat--warn' : ''}`}>
      <span className="stat__label">{label}</span>
      <span className="stat__value">{value}</span>
    </div>
  )
}

export function StatusBar() {
  const tel = useStore((s) => s.telemetry)
  const requestForceRestart = useStore((s) => s.forceRestartEngine)
  const [restarting, setRestarting] = useState(false)
  const [restartError, setRestartError] = useState<string | null>(null)
  const { t } = useTranslation()

  const forceRestart = async () => {
    if (!window.confirm(t('status.forceRestartConfirm'))) return
    setRestarting(true)
    setRestartError(null)
    try {
      await requestForceRestart()
    } catch (error) {
      setRestartError((error as Error).message)
    } finally {
      setRestarting(false)
    }
  }

  if (!tel) {
    return (
      <footer className="status">
        <Stat label={t('status.engine')} value={t('status.connecting')} warn />
      </footer>
    )
  }

  return (
    <footer className="status">
      <Stat label={t('status.block')} value={`${tel.quantum} @ ${(tel.sampleRate / 1000).toFixed(1)} kHz`} />
      <Stat
        label={t('status.latency')}
        value={t('status.latencyValue', {
          total: tel.totalLatencyMs.toFixed(2),
          graph: tel.graphLatencyMs.toFixed(2),
        })}
        warn={tel.totalLatencyMs > 20}
      />
      <Stat
        label={t('status.dsp')}
        value={t('status.dspValue', {
          avg: tel.dspUsAvg.toFixed(1),
          peak: tel.dspUs.toFixed(1),
          load: tel.loadAvg.toFixed(1),
        })}
        warn={tel.loadAvg > 50}
      />
      <Stat label={t('status.xruns')} value={String(tel.xruns)} warn={tel.xruns > 0} />
      <Stat
        label={t('status.buffers')}
        value={t('status.buffersValue', { slots: tel.bufferSlots, nodes: tel.nodes })}
      />
      <Stat
        label={t('status.graph')}
        value={t('status.graphValue', { gen: tel.generation, swaps: tel.swaps })}
      />
      <Stat
        label={t('status.scheduling')}
        value={tel.realtime ? t('status.realtime') : t('status.notRealtime')}
        warn={!tel.realtime}
      />
      <span className="status__spacer" />
      {restartError && <span className="field__error">{restartError}</span>}
      <button
        className="button button--small button--danger"
        disabled={restarting}
        title={t('status.forceRestartHint')}
        onClick={() => void forceRestart()}
      >
        {restarting ? t('status.forceRestarting') : t('status.forceRestart')}
      </button>
    </footer>
  )
}
