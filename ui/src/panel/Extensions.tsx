import { useTranslation } from 'react-i18next'
import type { TFunction } from 'i18next'

import { useStore } from '../store'
import { desktopBridgeAvailable, type Extension } from '../api'
import { ExtensionConfig } from './ExtensionConfig'
import { extensionNodeType, extensionTranslationKey } from '../i18n'

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
  const extensionId = extension.id || extension.key
  const name = t(extensionTranslationKey(extensionId, 'name'), {
    defaultValue: extension.name || extension.key,
  })
  const description = t(extensionTranslationKey(extensionId, 'description'), {
    defaultValue: extension.description,
  })
  const translatedTypes = inUse.map((type) => t(
    extensionTranslationKey(
      extensionId,
      `nodes.${extensionNodeType(extensionId, type)}.label`,
    ),
    { defaultValue: type },
  ))

  return (
    <section
      className={`extcard${selected === extension.key ? ' extcard--on' : ''}`}
      onClick={() => selectExtension(extension.key)}
    >
      <header className="extcard__head">
        <h3 className="extcard__name">{name}</h3>
        {extension.version && <span className="extcard__version">{extension.version}</span>}
        <span className="spacer" />
        <span className={`extcard__badge extcard__badge--${badge.tone}`}>{badge.label}</span>
      </header>

      <p className="extcard__where">
        <code>{extension.id || extension.key}</code>
        <span>{extension.path}</span>
      </p>

      {description && <p className="hint">{description}</p>}

      {extension.error && (
        <p className="banner banner--error extcard__error">{extension.error}</p>
      )}

      {extension.nodeTypes.length > 0 && (
        <div className="chips">
          {extension.nodeTypes.map((type) => (
            <span key={type} className="chip">
              {t(
                extensionTranslationKey(
                  extensionId,
                  `nodes.${extensionNodeType(extensionId, type)}.label`,
                ),
                { defaultValue: type },
              )}
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
              {t(extensionTranslationKey(extensionId, `presets.${preset.name}`), {
                defaultValue: preset.name,
              })}
            </button>
          ))}
        </div>
      )}

      {isOn && inUse.length > 0 && (
        <p className="hint hint--strong">
          {t('extensions.inUse', { types: translatedTypes.join(', ') })}
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
  const chooseExtension = useStore((s) => s.chooseExtension)

  return (
    <div className="page">
      <div className="page__main">
        <div className="panel">
          <h2 className="panel__title">{t('extensions.title')}</h2>

          <div className="extensions__tools">
            <button className="button button--small" disabled={busy} onClick={() => void rescanExtensions()}>
              {t('extensions.rescan')}
            </button>
            {desktopBridgeAvailable && (
              <button
                className="button button--small"
                disabled={busy}
                onClick={() => void chooseExtension()}
              >
                {t('extensions.choose')}
              </button>
            )}
          </div>
          <p className="hint">{t(desktopBridgeAvailable ? 'extensions.chooseHint' : 'extensions.rescanHint')}</p>
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
