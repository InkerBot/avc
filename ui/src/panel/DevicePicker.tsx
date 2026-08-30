import { useEffect, useMemo, useRef, useState } from 'react'
import { useTranslation } from 'react-i18next'
import type { TFunction } from 'i18next'
import { useStore } from '../store'
import type { AudioDevice } from '../api'

type Kind = 'input' | 'output' | 'app' | 'published'

function kindOf(device: AudioDevice, published: Set<string>): Kind {
  if (published.has(device.name)) return 'published'
  if (device.mediaClass.startsWith('Stream/')) return 'app'
  if (device.mediaClass === 'Audio/Sink') return 'output'
  return 'input'
}

function titleOf(device: AudioDevice): string {
  if (device.mediaClass.startsWith('Stream/')) {
    const what = device.mediaName && device.mediaName !== device.application ? device.mediaName : ''
    return [device.application || device.name, what].filter(Boolean).join(' — ')
  }
  return device.description || device.name
}

interface Row {
  device: AudioDevice
  kind: Kind
  title: string
  detail: string
  channels: number
}

function groupsFor(rows: Row[], role: 'capture' | 'playback', t: TFunction) {
  if (role === 'playback') {
    return [{ name: t('picker.playTo'), hint: '', items: rows.filter((r) => r.kind === 'output') }]
  }
  return [
    { name: t('picker.micsAndInputs'), hint: '', items: rows.filter((r) => r.kind === 'input') },
    {
      name: t('picker.outputs'),
      hint: t('picker.outputsHint'),
      items: rows.filter((r) => r.kind === 'output'),
    },
    {
      name: t('picker.apps'),
      hint: t('picker.appsHint'),
      items: rows.filter((r) => r.kind === 'app'),
    },
  ]
}

export function DevicePicker({
  role,
  value,
  onChange,
}: {
  role: 'capture' | 'playback'
  value: string
  onChange: (name: string) => void
}) {
  const devices = useStore((s) => s.audioDevices)
  const nodes = useStore((s) => s.nodes)
  const [open, setOpen] = useState(false)
  const [filter, setFilter] = useState('')
  const box = useRef<HTMLDivElement>(null)
  const { t } = useTranslation()

  const kindLabel: Record<Kind, string> = {
    input: t('picker.kind.mic'),
    output: t('picker.kind.out'),
    app: t('picker.kind.app'),
    published: t('picker.kind.published'),
  }

  const published = useMemo(
    () =>
      new Set(
        nodes
          .map((n) => n.data.options?.publish_as)
          .filter((name): name is string => Boolean(name)),
      ),
    [nodes],
  )

  const rows: Row[] = useMemo(() => {
    const wanted = role === 'capture' ? 'outputPorts' : 'inputPorts'
    const usable = devices
      .filter((d) => d[wanted] > 0 && !published.has(d.name) && d.mediaClass !== 'Audio/Filter')
      .filter((d) => (role === 'playback' ? d.mediaClass === 'Audio/Sink' : true))

    const seen = new Map<string, number>()
    usable.forEach((d) => seen.set(titleOf(d), (seen.get(titleOf(d)) ?? 0) + 1))

    return usable.map((d) => ({
      device: d,
      kind: kindOf(d, published),
      title: titleOf(d),
      detail: (seen.get(titleOf(d)) ?? 0) > 1 ? `${d.name} · #${d.id}` : d.name,
      channels: d[wanted],
    }))
  }, [devices, published, role])

  const needle = filter.trim().toLowerCase()
  const groups = groupsFor(
    needle
      ? rows.filter(
          (r) =>
            r.title.toLowerCase().includes(needle) || r.device.name.toLowerCase().includes(needle),
        )
      : rows,
    role,
    t,
  ).filter((g) => g.items.length > 0)

  useEffect(() => {
    if (!open) return
    const close = (e: MouseEvent) => {
      if (box.current && !box.current.contains(e.target as Node)) setOpen(false)
    }
    document.addEventListener('mousedown', close)
    return () => document.removeEventListener('mousedown', close)
  }, [open])

  const selected = rows.find((r) => r.device.name === value)
  const following = value.startsWith('@')

  return (
    <div className="picker" ref={box}>
      <button
        type="button"
        className={`picker__trigger${value ? '' : ' picker__trigger--empty'}`}
        onClick={() => {
          setOpen((v) => !v)
          setFilter('')
        }}
        aria-expanded={open}
      >
        <span className="picker__face">
          {following ? (
            <>
              <span className="picker__badge picker__badge--follow">{t('picker.auto')}</span>
              <span className="picker__title">
                {value === '@default_source' ? t('picker.defaultInput') : t('picker.defaultOutput')}
              </span>
              <span className="picker__sub">{t('picker.followsSystem')}</span>
            </>
          ) : selected ? (
            <>
              <span className={`picker__badge picker__badge--${selected.kind}`}>
                {kindLabel[selected.kind]}
              </span>
              <span className="picker__title">{selected.title}</span>
              <span className="picker__sub">
                {t('picker.channels', { count: selected.channels })}
                {selected.kind === 'output' && role === 'capture'
                  ? ` · ${t('picker.whatPlaying')}`
                  : ''}
              </span>
            </>
          ) : value ? (
            <>
              <span className="picker__badge picker__badge--gone">{t('picker.gone')}</span>
              <span className="picker__title">{value}</span>
              <span className="picker__sub">{t('picker.notHere')}</span>
            </>
          ) : (
            <span className="picker__title picker__title--empty">
              {role === 'capture' ? t('picker.chooseCapture') : t('picker.choosePlayback')}
            </span>
          )}
        </span>
        <span className="picker__caret">▾</span>
      </button>

      {open && (
        <div className="picker__panel">
          <input
            className="picker__filter"
            autoFocus
            value={filter}
            placeholder={t('picker.filter')}
            onChange={(e) => setFilter(e.target.value)}
            onKeyDown={(e) => e.key === 'Escape' && setOpen(false)}
          />

          <button
            type="button"
            className={`picker__row${following ? ' picker__row--on' : ''}`}
            onClick={() => {
              onChange(role === 'capture' ? '@default_source' : '@default_sink')
              setOpen(false)
            }}
          >
            <span className="picker__badge picker__badge--follow">{t('picker.auto')}</span>
            <span className="picker__row-body">
              <span className="picker__row-title">
                {role === 'capture' ? t('picker.defaultInput') : t('picker.defaultOutput')}
              </span>
              <span className="picker__row-sub">{t('picker.followsSystem')}</span>
            </span>
          </button>

          {groups.map((group) => (
            <div key={group.name}>
              <div className="picker__group">
                {group.name}
                {group.hint && <em> — {group.hint}</em>}
              </div>
              {group.items.map((row) => (
                <button
                  type="button"
                  key={row.device.name}
                  className={`picker__row${row.device.name === value ? ' picker__row--on' : ''}`}
                  onClick={() => {
                    onChange(row.device.name)
                    setOpen(false)
                  }}
                >
                  <span className={`picker__badge picker__badge--${row.kind}`}>
                    {kindLabel[row.kind]}
                  </span>
                  <span className="picker__row-body">
                    <span className="picker__row-title">{row.title}</span>
                    <span className="picker__row-sub">{row.detail}</span>
                  </span>
                  <span className="picker__row-ch">{t('picker.channels', { count: row.channels })}</span>
                </button>
              ))}
            </div>
          ))}

          {groups.length === 0 && <p className="hint picker__empty">{t('picker.nothing')}</p>}
        </div>
      )}
    </div>
  )
}
