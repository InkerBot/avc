import { useStore } from '../store'
import type { AudioDevice } from '../api'
import { useTranslation } from 'react-i18next'
import type { TFunction } from 'i18next'

function group(devices: AudioDevice[], t: TFunction) {
  const label = (d: AudioDevice) =>
    d.mediaClass === 'Stream/Output/Audio' || d.mediaClass === 'Stream/Input/Audio'
      ? `${d.application || d.name}${d.mediaName && d.mediaName !== d.application ? ` — ${d.mediaName}` : ''}`
      : d.description || d.name

  return [
    {
      name: t('devices.playing'),
      items: devices.filter((d) => d.mediaClass === 'Stream/Output/Audio'),
    },
    {
      name: t('devices.inputs'),
      items: devices.filter((d) => d.mediaClass.startsWith('Audio/Source')),
    },
    {
      name: t('devices.outputs'),
      items: devices.filter((d) => d.mediaClass === 'Audio/Sink'),
    },
  ].map((g) => ({ ...g, label }))
}

export function AudioDevices() {
  const devices = useStore((s) => s.audioDevices)
  const { t } = useTranslation()

  return (
    <div className="panel">
      <h2 className="panel__title">{t('devices.title')}</h2>
      {group(devices, t)
        .filter((g) => g.items.length > 0)
        .map((g) => (
          <section key={g.name} className="palette__group">
            <h3 className="palette__category">{g.name}</h3>
            <ul className="endpoints">
              {g.items.map((d) => (
                <li className="endpoint" key={d.name} title={d.name}>
                  <span className="endpoint__index">
                    {d.outputPorts > 0 ? d.outputPorts : d.inputPorts}
                  </span>
                  <span className="endpoint__name">{g.label(d)}</span>
                </li>
              ))}
            </ul>
          </section>
        ))}
    </div>
  )
}
