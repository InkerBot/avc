import { useTranslation } from 'react-i18next'
import { levelColor } from '../theme'

const kFloorDb = -60
const kCeilDb = 6

function bandFraction(db: number): number {
  return Math.max(0, Math.min(1, (db - kFloorDb) / (kCeilDb - kFloorDb)))
}

export function Scope({ wave, bands, bandsHz }: {
  wave: number[]
  bands: number[]
  bandsHz: number[]
}) {
  const { t } = useTranslation()
  const width = 160
  const waveHeight = 38
  const specHeight = 30

  const points = wave
    .map((v, i) => {
      const x = (i / Math.max(wave.length - 1, 1)) * width
      const y = waveHeight / 2 - Math.max(-1, Math.min(1, v)) * (waveHeight / 2 - 1)
      return `${x.toFixed(1)},${y.toFixed(1)}`
    })
    .join(' ')

  const peakDb = bands.length > 0 ? Math.max(...bands) : kFloorDb
  const barWidth = width / Math.max(bands.length, 1)

  return (
    <div className="scope">
      <svg className="scope__wave" viewBox={`0 0 ${width} ${waveHeight}`} preserveAspectRatio="none">
        <line x1="0" y1={waveHeight / 2} x2={width} y2={waveHeight / 2} className="scope__axis" />
        <polyline points={points} className="scope__trace" />
      </svg>

      <svg
        className="scope__spectrum"
        viewBox={`0 0 ${width} ${specHeight}`}
        preserveAspectRatio="none"
        role="img"
        aria-label={t('node.spectrum')}
      >
        {bands.map((db, i) => {
          const h = bandFraction(db) * specHeight
          return (
            <rect
              key={i}
              x={i * barWidth + 0.4}
              y={specHeight - h}
              width={Math.max(barWidth - 0.8, 0.6)}
              height={h}
              fill={levelColor(db)}
            >
              <title>{`${Math.round(bandsHz[i] ?? 0)} Hz · ${db.toFixed(0)} dB`}</title>
            </rect>
          )
        })}
      </svg>

      <div className="scope__legend">
        <span>{t('node.scopePeak', { db: peakDb.toFixed(0) })}</span>
        <span>
          {Math.round(bandsHz[0] ?? 0)}–{Math.round((bandsHz[bandsHz.length - 1] ?? 0) / 1000)}k Hz
        </span>
      </div>
    </div>
  )
}
