import {
  useEffect,
  useId,
  useMemo,
  useRef,
  useState,
  type KeyboardEvent as ReactKeyboardEvent,
} from 'react'
import { useTranslation } from 'react-i18next'
import type { TFunction } from 'i18next'
import { useStore } from '../store'
import type { AudioDevice } from '../api'

type Kind = 'input' | 'output' | 'app'

function kindOf(device: AudioDevice): Kind {
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
  const refreshAudioDevices = useStore((s) => s.refreshAudioDevices)
  const nodes = useStore((s) => s.nodes)
  const [open, setOpen] = useState(false)
  const [filter, setFilter] = useState('')
  const [refreshing, setRefreshing] = useState(false)
  const [refreshError, setRefreshError] = useState<string | null>(null)
  const box = useRef<HTMLDivElement>(null)
  const trigger = useRef<HTMLButtonElement>(null)
  const panel = useRef<HTMLDivElement>(null)
  const panelId = useId()
  const [placement, setPlacement] = useState({ left: 0, top: 0, width: 280, maxHeight: 360 })
  const { t } = useTranslation()

  const kindLabel: Record<Kind, string> = {
    input: t('picker.kind.mic'),
    output: t('picker.kind.out'),
    app: t('picker.kind.app'),
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
      kind: kindOf(d),
      title: titleOf(d),
      detail: (seen.get(titleOf(d)) ?? 0) > 1 ? `${d.name} · #${d.id}` : '',
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

  useEffect(() => {
    if (!open) return
    const place = () => {
      const rect = trigger.current?.getBoundingClientRect()
      if (!rect) return
      const gap = 6
      const roomBelow = window.innerHeight - rect.bottom - gap
      const roomAbove = rect.top - gap
      const maxHeight = Math.max(180, Math.min(420, Math.max(roomBelow, roomAbove) - 8))
      const openAbove = roomBelow < 260 && roomAbove > roomBelow
      setPlacement({
        left: Math.max(8, Math.min(rect.left, window.innerWidth - Math.max(rect.width, 280) - 8)),
        top: openAbove ? Math.max(8, rect.top - maxHeight - gap) : rect.bottom + gap,
        width: Math.max(rect.width, 280),
        maxHeight,
      })
    }
    place()
    window.addEventListener('resize', place)
    window.addEventListener('scroll', place, true)
    return () => {
      window.removeEventListener('resize', place)
      window.removeEventListener('scroll', place, true)
    }
  }, [open])

  const close = (restoreFocus = false) => {
    setOpen(false)
    if (restoreFocus) window.setTimeout(() => trigger.current?.focus())
  }

  const openPicker = () => {
    setOpen(true)
    setFilter('')
    setRefreshing(true)
    setRefreshError(null)
    void refreshAudioDevices()
      .catch((error: Error) => setRefreshError(error.message))
      .finally(() => setRefreshing(false))
  }

  const navigateOptions = (event: ReactKeyboardEvent) => {
    if (event.defaultPrevented) return
    if (event.key === 'Escape') {
      event.preventDefault()
      close(true)
      return
    }
    if (!['ArrowDown', 'ArrowUp', 'Home', 'End'].includes(event.key)) return
    const options = Array.from(panel.current?.querySelectorAll<HTMLButtonElement>('.picker__row') ?? [])
    if (options.length === 0) return
    event.preventDefault()
    const current = options.indexOf(document.activeElement as HTMLButtonElement)
    const next = event.key === 'Home'
      ? 0
      : event.key === 'End'
        ? options.length - 1
        : event.key === 'ArrowDown'
          ? Math.min(options.length - 1, current + 1)
          : Math.max(0, current < 0 ? options.length - 1 : current - 1)
    options[next].focus()
  }

  const selected = rows.find((r) => r.device.name === value)
  const following = value.startsWith('@')
  const unavailable = Boolean(value && !following && !selected)

  return (
    <div className="picker" ref={box}>
      <button
        ref={trigger}
        type="button"
        className={`picker__trigger${value ? '' : ' picker__trigger--empty'}${unavailable ? ' picker__trigger--gone' : ''}`}
        onClick={() => {
          if (open) close()
          else openPicker()
        }}
        onKeyDown={(event) => {
          if (event.key === 'ArrowDown' && !open) {
            event.preventDefault()
            openPicker()
          } else if (event.key === 'Escape' && open) {
            event.preventDefault()
            close(true)
          }
        }}
        aria-expanded={open}
        aria-haspopup="dialog"
        aria-controls={open ? panelId : undefined}
        aria-invalid={unavailable}
        title={unavailable ? t('picker.notHere') : undefined}
      >
        <span className="picker__face">
          {following ? (
            <>
              <span className="picker__badge picker__badge--follow">{t('picker.auto')}</span>
              <span className="picker__title">
                {value === '@default_source' ? t('picker.defaultInput') : t('picker.defaultOutput')}
              </span>
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
        <div
          ref={panel}
          id={panelId}
          className="picker__panel"
          role="dialog"
          aria-label={role === 'capture' ? t('picker.chooseCapture') : t('picker.choosePlayback')}
          aria-busy={refreshing}
          style={placement}
          onKeyDown={navigateOptions}
        >
          <input
            className="picker__filter"
            autoFocus
            value={filter}
            placeholder={t('picker.filter')}
            onChange={(e) => setFilter(e.target.value)}
            onKeyDown={navigateOptions}
          />

          {refreshing && (
            <p className="hint picker__notice" role="status">{t('picker.refreshing')}</p>
          )}
          {refreshError && (
            <p className="field__error picker__notice" role="alert">
              {t('picker.refreshFailed', { error: refreshError })}
            </p>
          )}

          <button
            type="button"
            className={`picker__row${following ? ' picker__row--on' : ''}`}
            aria-pressed={following}
            onClick={() => {
              onChange(role === 'capture' ? '@default_source' : '@default_sink')
              close(true)
            }}
          >
            <span className="picker__badge picker__badge--follow">{t('picker.auto')}</span>
            <span className="picker__row-body">
              <span className="picker__row-title">
                {role === 'capture' ? t('picker.defaultInput') : t('picker.defaultOutput')}
              </span>
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
                  aria-pressed={row.device.name === value}
                  onClick={() => {
                    onChange(row.device.name)
                    close(true)
                  }}
                >
                  <span className={`picker__badge picker__badge--${row.kind}`}>
                    {kindLabel[row.kind]}
                  </span>
                  <span className="picker__row-body">
                    <span className="picker__row-title">{row.title}</span>
                    {row.detail && <span className="picker__row-sub">{row.detail}</span>}
                  </span>
                  <span className="picker__row-ch">{t('picker.channels', { count: row.channels })}</span>
                </button>
              ))}
            </div>
          ))}

          {groups.length === 0 && !refreshing && (
            <p className="hint picker__empty">{t('picker.nothing')}</p>
          )}
        </div>
      )}
    </div>
  )
}
