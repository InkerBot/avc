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
  const { t } = useTranslation()

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
    </footer>
  )
}
