import { useEffect, useState } from 'react'
import { useStore } from '../store'
import { api, type AudioDevice, type UsbIpDriverStatus } from '../api'
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
  const refreshAudioDevices = useStore((s) => s.refreshAudioDevices)
  const [usbIp, setUsbIp] = useState<UsbIpDriverStatus | null>(null)
  const [driverBusy, setDriverBusy] = useState(false)
  const [driverError, setDriverError] = useState<string | null>(null)
  const { t } = useTranslation()

  useEffect(() => {
    let alive = true
    api.usbIpDriverStatus()
      .then((status) => { if (alive) setUsbIp(status) })
      .catch((error: Error) => { if (alive) setDriverError(error.message) })
    return () => { alive = false }
  }, [])

  const installDriver = async () => {
    if (!window.confirm(t('devices.usbipInstallConfirm'))) return
    setDriverBusy(true)
    setDriverError(null)
    try {
      setUsbIp(await api.installUsbIpDriver())
      void refreshAudioDevices()
    } catch (error) {
      setDriverError((error as Error).message)
    } finally {
      setDriverBusy(false)
    }
  }

  const driverState = usbIp?.driverReady
    ? t('devices.usbipReady', { version: usbIp.version ? ` · ${usbIp.version}` : '' })
    : usbIp?.clientPresent && !usbIp.compatible
      ? t('devices.usbipUnsupported', { version: usbIp.version ?? '?' })
      : usbIp?.clientPresent
        ? t('devices.usbipNotReady')
        : t('devices.usbipMissing')

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
      {usbIp?.supported && (
        <section className="usbip-driver">
          <h3 className="palette__category">{t('devices.usbipTitle')}</h3>
          <div className="usbip-driver__row">
            <span className={`usbip-driver__state${usbIp.driverReady ? ' usbip-driver__state--ready' : ''}`}>
              {driverState}
            </span>
            {usbIp.installerAvailable && (
              <button
                className="button button--small"
                disabled={driverBusy}
                onClick={() => void installDriver()}
              >
                {driverBusy
                  ? t('devices.usbipInstalling')
                  : usbIp.clientPresent
                    ? t('devices.usbipRepair')
                    : t('devices.usbipInstall')}
              </button>
            )}
          </div>
          {!usbIp.installerAvailable && !usbIp.driverReady && (
            <p className="hint">{t('devices.usbipInstallerMissing')}</p>
          )}
          {(driverError || usbIp.error) && (
            <span className="field__error">{driverError ?? usbIp.error}</span>
          )}
        </section>
      )}
    </div>
  )
}
