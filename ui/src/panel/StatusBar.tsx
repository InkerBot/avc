import { useState } from 'react'
import { useStore } from '../store'
import { useTranslation } from 'react-i18next'
import { ConfirmDialog } from '../components/ConfirmDialog'

function Stat({
  label,
  value,
  warn,
  secondary,
  adaptive,
}: {
  label: string
  value: string
  warn?: boolean
  secondary?: boolean
  adaptive?: boolean
}) {
  return (
    <div className={`stat${warn ? ' stat--warn' : ''}${secondary ? ' stat--secondary' : ''}${adaptive ? ' stat--adaptive' : ''}`}>
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
  const [confirmRestart, setConfirmRestart] = useState(false)
  const { t } = useTranslation()

  const forceRestart = async () => {
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

  const blockValue = `${tel.quantum} @ ${(tel.sampleRate / 1000).toFixed(1)} kHz`
  const buffersValue = t('status.buffersValue', { slots: tel.bufferSlots, nodes: tel.nodes })
  const graphValue = t('status.graphValue', { gen: tel.generation, swaps: tel.swaps })
  const schedulingValue = tel.realtime ? t('status.realtime') : t('status.notRealtime')

  return (
    <footer className="status">
      <Stat label={t('status.block')} value={blockValue} adaptive />
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
        value={buffersValue}
        secondary
      />
      <Stat
        label={t('status.graph')}
        value={graphValue}
        secondary
      />
      <Stat
        label={t('status.scheduling')}
        value={schedulingValue}
        warn={!tel.realtime}
        secondary
      />
      <details className="status__details">
        <summary className="button button--small">{t('status.more')}</summary>
        <div className="status__details-panel">
          <Stat label={t('status.block')} value={blockValue} />
          <Stat label={t('status.buffers')} value={buffersValue} />
          <Stat label={t('status.graph')} value={graphValue} />
          <Stat
            label={t('status.scheduling')}
            value={schedulingValue}
            warn={!tel.realtime}
          />
          {restartError && <span className="field__error">{restartError}</span>}
          <button
            className="button button--small button--danger"
            disabled={restarting}
            onClick={() => setConfirmRestart(true)}
          >
            {restarting ? t('status.forceRestarting') : t('status.forceRestart')}
          </button>
        </div>
      </details>
      <span className="status__spacer" />
      <ConfirmDialog
        open={confirmRestart}
        title={t('status.forceRestart')}
        confirmLabel={t('status.forceRestart')}
        cancelLabel={t('app.cancel')}
        confirmTone="danger"
        onCancel={() => setConfirmRestart(false)}
        onConfirm={() => {
          setConfirmRestart(false)
          void forceRestart()
        }}
      >
        <p>{t('status.forceRestartConfirm')}</p>
      </ConfirmDialog>
    </footer>
  )
}
