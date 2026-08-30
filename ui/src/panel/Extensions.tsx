import { useRef } from 'react'
import { useTranslation } from 'react-i18next'
import type { TFunction } from 'i18next'

import { useStore } from '../store'
import type { Extension } from '../api'
import { ExtensionConfig } from './ExtensionConfig'

function status(t: TFunction, extension: Extension): { label: string; tone: string } {
  if (extension.disabledReason === 'crash') {
    return { label: t('extensions.state.crashed'), tone: 'bad' }
  }
  if (!extension.enabled) return { label: t('extensions.state.disabled'), tone: 'off' }
  if (extension.state === 'failed') return { label: t('extensions.state.failed'), tone: 'bad' }
  if (extension.state === 'loaded') return { label: t('extensions.state.loaded'), tone: 'ok' }
  return { label: t('extensions.state.pending'), tone: 'off' }
}

function typesInUse(extension: Extension, nodeTypes: string[]): string[] {
  return extension.nodeTypes.filter((type) => nodeTypes.includes(type))
}

function Card({ extension }: { extension: Extension }) {
  const { t } = useTranslation()
  const selected = useStore((s) => s.selectedExtension)
  const busy = useStore((s) => s.extensionsBusy)
  const nodes = useStore((s) => s.nodes)
  const selectExtension = useStore((s) => s.selectExtension)
  const setExtensionEnabled = useStore((s) => s.setExtensionEnabled)
  const deleteExtension = useStore((s) => s.deleteExtension)
  const loadExtensionPreset = useStore((s) => s.loadExtensionPreset)

  const badge = status(t, extension)
  const inUse = typesInUse(
    extension,
    nodes.map((n) => n.data.type),
  )
  const isOn = extension.enabled

  return (
    <section
      className={`extcard${selected === extension.key ? ' extcard--on' : ''}`}
      onClick={() => selectExtension(extension.key)}
    >
      <header className="extcard__head">
        <h3 className="extcard__name">{extension.name || extension.key}</h3>
        {extension.version && <span className="extcard__version">{extension.version}</span>}
        <span className="spacer" />
        <span className={`extcard__badge extcard__badge--${badge.tone}`}>{badge.label}</span>
      </header>

      <p className="extcard__where">
        <code>{extension.id || extension.key}</code>
        <span>{extension.path}</span>
      </p>

      {extension.description && <p className="hint">{extension.description}</p>}

      {extension.error && (
        <p className="banner banner--error extcard__error">{extension.error}</p>
      )}

      {extension.nodeTypes.length > 0 && (
        <div className="chips">
          {extension.nodeTypes.map((type) => (
            <span key={type} className="chip">
              {type}
            </span>
          ))}
        </div>
      )}

      {extension.presets.length > 0 && (
        <div className="chips">
          {extension.presets.map((preset) => (
            <button
              key={preset.name}
              className="chip chip--action"
              disabled={!isOn || extension.state !== 'loaded'}
              title={t('extensions.loadGraphHint')}
              onClick={(e) => {
                e.stopPropagation()
                void loadExtensionPreset(extension.key, preset.name)
              }}
            >
              {preset.name}
            </button>
          ))}
        </div>
      )}

      {isOn && inUse.length > 0 && (
        <p className="hint hint--strong">
          {t('extensions.inUse', { types: inUse.join(', ') })}
        </p>
      )}

      <footer className="extcard__foot">
        <button
          className={`button button--small${isOn ? '' : ' button--primary'}`}
          disabled={busy}
          title={t('extensions.restartNote')}
          onClick={(e) => {
            e.stopPropagation()
            void setExtensionEnabled(extension.key, !isOn)
          }}
        >
          {isOn ? t('extensions.disable') : t('extensions.enable')}
        </button>
        <button
          className="button button--small"
          disabled={busy || isOn}
          title={isOn ? t('extensions.deleteNeedsDisable') : t('extensions.deleteHint')}
          onClick={(e) => {
            e.stopPropagation()
            void deleteExtension(extension.key)
          }}
        >
          {t('extensions.delete')}
        </button>
      </footer>
    </section>
  )
}

export function Extensions() {
  const { t } = useTranslation()
  const extensions = useStore((s) => s.extensions)
  const busy = useStore((s) => s.extensionsBusy)
  const error = useStore((s) => s.extensionError)
  const rescanExtensions = useStore((s) => s.rescanExtensions)
  const installExtension = useStore((s) => s.installExtension)
  const file = useRef<HTMLInputElement>(null)

  return (
    <div className="page">
      <div className="page__main">
        <div className="panel">
          <h2 className="panel__title">{t('extensions.title')}</h2>

          <div className="extensions__tools">
            <button className="button button--small" disabled={busy} onClick={() => void rescanExtensions()}>
              {t('extensions.rescan')}
            </button>
            <button
              className="button button--small"
              disabled={busy}
              onClick={() => file.current?.click()}
            >
              {t('extensions.upload')}
            </button>
            <input
              ref={file}
              type="file"
              accept=".so"
              hidden
              onChange={(e) => {
                const chosen = e.target.files?.[0]
                if (chosen) void installExtension(chosen)
                e.target.value = ''
              }}
            />
          </div>
          <p className="hint">{t('extensions.uploadHint')}</p>
          {error && <p className="banner banner--error">{error}</p>}
        </div>

        <div className="extensions__list">
          {extensions.length === 0 ? (
            <p className="hint">{t('extensions.empty')}</p>
          ) : (
            extensions.map((extension) => <Card key={extension.key} extension={extension} />)
          )}
        </div>
      </div>

      <aside className="side side--right">
        <ExtensionConfig />
      </aside>
    </div>
  )
}
