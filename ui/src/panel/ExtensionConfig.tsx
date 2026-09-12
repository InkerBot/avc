import { useEffect, useState } from 'react'
import { useTranslation } from 'react-i18next'
import type { TFunction } from 'i18next'

import { useStore } from '../store'
import type { Extension, ExtensionSetting } from '../api'
import { ExtensionComponent } from '../extensions/ExtensionComponent'
import { useExtensionContext } from '../extensions/context'
import { extensionTranslationKey } from '../i18n'

function settingLabel(t: TFunction, id: string, setting: ExtensionSetting): string {
  return t([
    extensionTranslationKey(id, `settings.${setting.key}.name`),
    `extensionSettings.${id}.${setting.key}.name`,
  ], {
    defaultValue: setting.label || setting.key.replace(/_/g, ' '),
  })
}

function settingHint(t: TFunction, id: string, setting: ExtensionSetting): string {
  return t([
    extensionTranslationKey(id, `settings.${setting.key}.description`),
    `extensionSettings.${id}.${setting.key}.description`,
  ], {
    defaultValue: setting.description ?? '',
  })
}

function Field({
  id,
  setting,
  value,
  onChange,
}: {
  id: string
  setting: ExtensionSetting
  value: string
  onChange: (v: string) => void
}) {
  const { t } = useTranslation()
  const label = settingLabel(t, id, setting)
  const hint = settingHint(t, id, setting)

  if (setting.type === 'bool') {
    return (
      <div className="field">
        <label className="field--check">
          <input
            type="checkbox"
            checked={value === '1' || value === 'true'}
            onChange={(e) => onChange(e.target.checked ? '1' : '0')}
          />
          <span>{label}</span>
        </label>
        {hint && <span className="hint">{hint}</span>}
      </div>
    )
  }

  if (setting.type === 'enum') {
    return (
      <label className="field">
        <span className="field__name">{label}</span>
        <select value={value} onChange={(e) => onChange(e.target.value)}>
          {setting.values.map((option) => (
            <option key={option} value={option}>
              {t([
                extensionTranslationKey(id, `settings.${setting.key}.values.${option}`),
                `extensionSettings.${id}.${setting.key}.values.${option}`,
              ], {
                defaultValue: option,
              })}
            </option>
          ))}
        </select>
        {hint && <span className="hint">{hint}</span>}
      </label>
    )
  }

  if (setting.type === 'int' || setting.type === 'float') {
    return (
      <label className="field">
        <span className="field__name">
          {label}
          <output>{value}</output>
        </span>
        <input
          type="number"
          value={value}
          min={setting.min}
          max={setting.max}
          step={setting.type === 'int' ? 1 : 'any'}
          onChange={(e) => onChange(e.target.value)}
        />
        {hint && <span className="hint">{hint}</span>}
      </label>
    )
  }

  return (
    <label className="field">
      <span className="field__name">{label}</span>
      <input
        value={value}
        placeholder={setting.type === 'path' ? t('extensions.pathPlaceholder') : undefined}
        onChange={(e) => onChange(e.target.value)}
      />
      {hint && <span className="hint">{hint}</span>}
    </label>
  )
}

function initial(extension: Extension): Record<string, string> {
  const values: Record<string, string> = {}
  extension.settings.forEach((setting) => {
    values[setting.key] = extension.values[setting.key] ?? setting.default
  })
  return values
}

function GenericExtensionConfig({ extension }: { extension?: Extension }) {
  const { t } = useTranslation()
  const busy = useStore((s) => s.extensionsBusy)
  const save = useStore((s) => s.saveExtensionSettings)
  const [draft, setDraft] = useState<Record<string, string>>({})
  const extensionId = extension?.id || extension?.key || ''
  const extensionName = extension
    ? t(extensionTranslationKey(extensionId, 'name'), {
        defaultValue: extension.name || extension.key,
      })
    : ''

  useEffect(() => {
    setDraft(extension ? initial(extension) : {})
  }, [extension])

  if (!extension) {
    return (
      <div className="panel">
        <h2 className="panel__title">{t('extensions.settingsTitle')}</h2>
        <p className="hint">{t('extensions.settingsEmpty')}</p>
      </div>
    )
  }

  if (extension.settings.length === 0) {
    return (
      <div className="panel">
        <h2 className="panel__title">{t('extensions.settingsTitle')}</h2>
        <p className="hint">
          {extension.state === 'loaded'
            ? t('extensions.settingsNone', { name: extensionName })
            : t('extensions.settingsUnknown')}
        </p>
      </div>
    )
  }

  const changed = extension.settings.some(
    (setting) => draft[setting.key] !== (extension.values[setting.key] ?? setting.default),
  )

  return (
    <div className="panel">
      <h2 className="panel__title">{t('extensions.settingsTitle')}</h2>
      <div className="inspector__head">
        <strong>{extensionName}</strong>
        <code>{extension.id}</code>
      </div>

      {extension.settings.map((setting) => (
        <Field
          key={setting.key}
          id={extension.id || extension.key}
          setting={setting}
          value={draft[setting.key] ?? setting.default}
          onChange={(v) => setDraft((d) => ({ ...d, [setting.key]: v }))}
        />
      ))}

      <button
        className="button button--primary"
        disabled={busy || !changed}
        onClick={() => void save(extension.key, draft)}
      >
        {t('extensions.save')}
      </button>
    </div>
  )
}

export function ExtensionConfig() {
  const key = useStore((s) => s.selectedExtension)
  const extension = useStore((s) => s.extensions.find((entry) => entry.key === key))
  const context = useExtensionContext(extension)

  if (!extension || !context) return <GenericExtensionConfig extension={extension} />
  return (
    <ExtensionComponent
      extension={extension}
      surface="settings"
      context={context}
      fallback={<GenericExtensionConfig extension={extension} />}
    />
  )
}
