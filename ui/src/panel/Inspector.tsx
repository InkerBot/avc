import { useEffect, useId, useRef, useState, type FormEvent } from 'react'
import {
  useStore,
  portCounts,
  COLD,
  COLD_BLOCKS,
  domainNameIssue,
  type DomainNameIssue,
} from '../store'
import { useTranslation } from 'react-i18next'
import type { TFunction } from 'i18next'
import { DevicePicker } from './DevicePicker'
import type { ParamDescriptor } from '../api'
import { colorOf } from '../theme'
import { ExtensionComponent } from '../extensions/ExtensionComponent'
import { useExtensionContext } from '../extensions/context'
import { extensionNodeType, extensionTranslationKey } from '../i18n'

function nodeTranslationKeys(
  extensionId: string,
  nodeType: string,
  suffix: string,
  fallback: string[] = [],
): string[] {
  return [
    ...(extensionId
      ? [
          extensionTranslationKey(
            extensionId,
            `nodes.${extensionNodeType(extensionId, nodeType)}.${suffix}`,
          ),
        ]
      : []),
    `nodes.${nodeType}.${suffix}`,
    ...fallback,
  ]
}

function paramLabel(
  t: TFunction,
  extensionId: string,
  nodeType: string,
  param: ParamDescriptor,
): string {
  return t(nodeTranslationKeys(
    extensionId,
    nodeType,
    `params.${param.name}.name`,
    [`params.${param.name}.name`],
  ), {
    defaultValue: param.name.replace(/_/g, ' '),
  })
}

function paramHint(
  t: TFunction,
  extensionId: string,
  nodeType: string,
  param: ParamDescriptor,
): string {
  return t(nodeTranslationKeys(extensionId, nodeType, `params.${param.name}.description`), {
    defaultValue: param.description ?? '',
  })
}

function ParamControl({
  nodeId,
  extensionId,
  nodeType,
  param,
  value,
  onChange,
}: {
  nodeId: string
  extensionId: string
  nodeType: string
  param: ParamDescriptor
  value: number
  onChange: (v: number) => void
}) {
  const { t } = useTranslation()
  const label = paramLabel(t, extensionId, nodeType, param)
  const hint = paramHint(t, extensionId, nodeType, param)

  if (param.type === 'bool') {
    return (
      <div className="field">
        <label className="field--check">
          <input
            type="checkbox"
            checked={value >= 0.5}
            onChange={(e) => onChange(e.target.checked ? 1 : 0)}
          />
          <span>{label}</span>
        </label>
        {hint && <span className="hint">{hint}</span>}
      </div>
    )
  }

  if (param.type === 'enum' && param.values) {
    return (
      <label className="field">
        <span className="field__name">{label}</span>
        <select value={value} onChange={(e) => onChange(Number(e.target.value))}>
          {param.values.map((valueLabel, i) => (
            <option key={valueLabel} value={i}>
              {t(
                [
                  ...nodeTranslationKeys(
                    extensionId,
                    nodeType,
                    `params.${param.name}.values.${valueLabel}`,
                  ),
                  `enumValues.${valueLabel}`,
                ],
                { defaultValue: valueLabel },
              )}
            </option>
          ))}
        </select>
      </label>
    )
  }

  const logarithmic = param.curve === 'log' && param.min > 0
  const toSlider = (v: number) =>
    logarithmic ? Math.log(v / param.min) / Math.log(param.max / param.min) : v
  const fromSlider = (t: number) =>
    logarithmic ? param.min * Math.pow(param.max / param.min, t) : t

  const min = logarithmic ? 0 : param.min
  const max = logarithmic ? 1 : param.max
  const step = (max - min) / 200

  return (
    <label className="field">
      <span className="field__name">
        {label}
        <output>
          {value.toFixed(Math.abs(value) < 10 ? 2 : Math.abs(value) < 1000 ? 1 : 0)}
          {param.unit && ` ${param.unit}`}
        </output>
      </span>
      <input
        type="range"
        min={min}
        max={max}
        step={step}
        value={toSlider(value)}
        onChange={(e) => onChange(fromSlider(Number(e.target.value)))}
        aria-label={`${nodeId} ${label}`}
      />
      {hint && <span className="hint">{hint}</span>}
    </label>
  )
}

function TargetControl({
  param,
  extensionId,
  nodeType,
  value,
  onChange,
  onEditStart,
  onEditEnd,
}: {
  param: ParamDescriptor
  extensionId: string
  nodeType: string
  value: string
  onChange: (v: string) => void
  onEditStart: () => void
  onEditEnd: () => void
}) {
  const { t } = useTranslation()
  const hint = paramHint(t, extensionId, nodeType, param)

  if (param.type === 'text' || param.type === 'path') {
    const path = param.type === 'path'
    return (
      <label className="field">
        <span className="field__name">{paramLabel(t, extensionId, nodeType, param)}</span>
        <input
          type="text"
          value={value}
          placeholder={t(path ? 'inspector.pathPlaceholder' : 'inspector.publishPlaceholder')}
          spellCheck={false}
          autoCapitalize="none"
          onFocus={onEditStart}
          onBlur={onEditEnd}
          onChange={(e) => onChange(e.target.value)}
        />
        {hint && <span className="hint">{hint}</span>}
      </label>
    )
  }

  return (
    <div className="field">
      <span className="field__name">{paramLabel(t, extensionId, nodeType, param)}</span>
      <DevicePicker
        role={param.deviceRole === 'playback' ? 'playback' : 'capture'}
        value={value}
        onChange={onChange}
      />
      {hint && <span className="hint">{hint}</span>}
    </div>
  )
}

function DomainControl({
  nodeId,
  domain,
  realtimeSafe,
  recommendedColdBlock,
}: {
  nodeId: string
  domain?: string
  realtimeSafe: boolean
  recommendedColdBlock: number
}) {
  const setDomain = useStore((s) => s.setDomain)
  const renameDomain = useStore((s) => s.renameDomain)
  const setDomainSetting = useStore((s) => s.setDomainSetting)
  const beginHistoryGroup = useStore((s) => s.beginHistoryGroup)
  const endHistoryGroup = useStore((s) => s.endHistoryGroup)
  const domains = useStore((s) => s.domains)
  const domainNodeCount = useStore((s) =>
    domain ? s.nodes.filter((node) => node.data.domain === domain).length : 0,
  )
  const settings = useStore((s) => s.domains[domain ?? COLD])
  const telemetry = useStore((s) => s.telemetry)
  const { t } = useTranslation()
  const selectId = useId()
  const createId = useId()
  const createErrorId = useId()
  const renameId = useId()
  const renameErrorId = useId()
  const [creating, setCreating] = useState(false)
  const [newName, setNewName] = useState('')
  const [createTouched, setCreateTouched] = useState(false)
  const [renameName, setRenameName] = useState(domain ?? '')
  const [renameTouched, setRenameTouched] = useState(false)
  const createInputRef = useRef<HTMLInputElement>(null)

  useEffect(() => {
    setRenameName(domain ?? '')
    setRenameTouched(false)
  }, [domain])

  useEffect(() => {
    if (creating) createInputRef.current?.focus()
  }, [creating])

  const cold = Boolean(domain)
  const domainNames = Object.keys(domains).sort((a, b) => a.localeCompare(b, 'zh-CN'))
  const rate = telemetry?.sampleRate || 48000
  const block = settings?.block ?? 1024
  const safety = settings?.safety ?? 1
  const addedMs = (1000 * block * (1 + safety)) / rate
  const blockOptions = Array.from(
    new Set([...COLD_BLOCKS, block, ...(recommendedColdBlock > 0 ? [recommendedColdBlock] : [])]),
  ).sort((a, b) => a - b)

  const issueText = (issue: DomainNameIssue | null) =>
    issue ? t(`inspector.domainNameError.${issue}`) : ''
  const createIssue = domainNameIssue(newName, domains)
  const renameIssue = domain ? domainNameIssue(renameName, domains, domain) : null

  const createPath = (event: FormEvent) => {
    event.preventDefault()
    setCreateTouched(true)
    if (createIssue) return
    setDomain(nodeId, newName.trim())
    setNewName('')
    setCreateTouched(false)
    setCreating(false)
  }

  const renamePath = (event: FormEvent) => {
    event.preventDefault()
    setRenameTouched(true)
    if (!domain || renameIssue) return
    renameDomain(domain, renameName)
  }

  return (
    <section className="field domain" aria-labelledby={`${selectId}-label`}>
      <label className="field__name" id={`${selectId}-label`} htmlFor={selectId}>
        {t('inspector.domain')}
      </label>
      <div className="domain__assign">
        <select
          id={selectId}
          value={domain ?? ''}
          onChange={(event) => setDomain(nodeId, event.target.value || undefined)}
        >
          <option value="" disabled={!realtimeSafe}>
            {t('inspector.hot')}
          </option>
          <optgroup label={t('inspector.coldPaths')}>
            {domainNames.map((name) => (
              <option key={name} value={name}>
                {name}
              </option>
            ))}
          </optgroup>
        </select>
        <button
          type="button"
          className="button button--small"
          aria-expanded={creating}
          aria-controls={`${createId}-form`}
          onClick={() => {
            setCreating((shown) => !shown)
            setCreateTouched(false)
          }}
        >
          {t('inspector.createColdPath')}
        </button>
      </div>
      <span className="hint">
        {!realtimeSafe
          ? t('inspector.notRealtimeSafe')
          : cold
            ? t('inspector.coldHintNamed', { name: domain })
            : t('inspector.hotHint')}
      </span>

      {creating && (
        <form id={`${createId}-form`} className="domain__form" onSubmit={createPath}>
          <label className="field__name" htmlFor={createId}>
            {t('inspector.coldPathName')}
          </label>
          <input
            ref={createInputRef}
            id={createId}
            type="text"
            value={newName}
            maxLength={48}
            autoComplete="off"
            spellCheck={false}
            aria-invalid={createTouched && Boolean(createIssue)}
            aria-describedby={createTouched && createIssue ? createErrorId : undefined}
            placeholder={t('inspector.coldPathPlaceholder')}
            onChange={(event) => setNewName(event.target.value)}
            onKeyDown={(event) => {
              if (event.key === 'Escape') {
                setCreating(false)
                setCreateTouched(false)
              }
            }}
          />
          {createTouched && createIssue && (
            <span className="field__error" id={createErrorId}>
              {issueText(createIssue)}
            </span>
          )}
          <div className="domain__actions">
            <button type="submit" className="button button--small">
              {t('inspector.createPath')}
            </button>
            <button
              type="button"
              className="button button--small"
              onClick={() => {
                setCreating(false)
                setCreateTouched(false)
              }}
            >
              {t('inspector.cancel')}
            </button>
          </div>
        </form>
      )}

      {cold && (
        <div className="domain__settings">
          <label className="field">
            <span className="field__name">{t('inspector.coldBlock')}</span>
            <select
              value={block}
              onChange={(e) => setDomainSetting(domain ?? COLD, 'block', Number(e.target.value))}
            >
              {blockOptions.map((frames) => (
                <option key={frames} value={frames}>
                  {t('inspector.coldBlockValue', {
                    frames,
                    ms: ((1000 * frames) / rate).toFixed(1),
                  })}
                </option>
              ))}
            </select>
          </label>
          {recommendedColdBlock > 0 && block < recommendedColdBlock && (
            <div className="hint hint--strong">
              <span>
                {t('inspector.coldBlockBelowRecommended', { frames: recommendedColdBlock })}
              </span>{' '}
              <button
                type="button"
                className="button button--small"
                onClick={() =>
                  setDomainSetting(domain ?? COLD, 'block', recommendedColdBlock)
                }
              >
                {t('inspector.useRecommendedColdBlock', { frames: recommendedColdBlock })}
              </button>
            </div>
          )}
          <label className="field">
            <span className="field__name">
              {t('inspector.coldSafety')}
              <output>{safety}</output>
            </span>
            <input
              type="range"
              min={0}
              max={8}
              step={1}
              value={safety}
              onPointerDown={beginHistoryGroup}
              onPointerUp={endHistoryGroup}
              onKeyDown={beginHistoryGroup}
              onKeyUp={endHistoryGroup}
              onChange={(e) => setDomainSetting(domain ?? COLD, 'safety', Number(e.target.value))}
            />
          </label>
          <span className="hint hint--strong">
            {t('inspector.coldCost', { ms: addedMs.toFixed(1) })}
          </span>
          <details className="domain__manage">
            <summary>{t('inspector.manageColdPath')}</summary>
            <p className="hint">
              {t('inspector.coldPathMembers', { count: domainNodeCount })}
            </p>
            <form className="domain__form domain__form--nested" onSubmit={renamePath}>
              <label className="field__name" htmlFor={renameId}>
                {t('inspector.renameColdPath')}
              </label>
              <input
                id={renameId}
                type="text"
                value={renameName}
                maxLength={48}
                autoComplete="off"
                spellCheck={false}
                aria-invalid={renameTouched && Boolean(renameIssue)}
                aria-describedby={renameTouched && renameIssue ? renameErrorId : undefined}
                onChange={(event) => setRenameName(event.target.value)}
              />
              {renameTouched && renameIssue && (
                <span className="field__error" id={renameErrorId}>
                  {issueText(renameIssue)}
                </span>
              )}
              <button
                type="submit"
                className="button button--small"
                disabled={renameName.trim() === domain}
              >
                {t('inspector.renamePath')}
              </button>
            </form>
            <p className="hint">{t('inspector.removeColdPathHint')}</p>
          </details>
        </div>
      )}
    </section>
  )
}

export function Inspector() {
  const selected = useStore((s) => s.selected)
  const node = useStore((s) => s.nodes.find((n) => n.id === selected))
  const descriptor = useStore((s) => (node ? s.descriptors[node.data.type] : undefined))
  const extension = useStore((s) => s.extensions.find((entry) => entry.id === descriptor?.extension))
  const setParam = useStore((s) => s.setParam)
  const setPortCount = useStore((s) => s.setPortCount)
  const setOption = useStore((s) => s.setOption)
  const beginHistoryGroup = useStore((s) => s.beginHistoryGroup)
  const endHistoryGroup = useStore((s) => s.endHistoryGroup)
  const { t } = useTranslation()
  const extensionContext = useExtensionContext(extension, node)

  if (!node || !descriptor) {
    return (
      <div className="panel">
        <h2 className="panel__title">{t('inspector.title')}</h2>
        <p className="hint">{t('inspector.empty')}</p>
      </div>
    )
  }

  const counts = portCounts(descriptor, node.data)
  const extensionName = descriptor.extension
    ? t(extensionTranslationKey(descriptor.extension, 'name'), {
        defaultValue: extension?.name || descriptor.extension,
      })
    : ''

  return (
    <div className="panel">
      <h2 className="panel__title">{t('inspector.title')}</h2>
      <div className="inspector__head" style={{ borderLeftColor: colorOf(descriptor.category) }}>
        <strong>
          {t(nodeTranslationKeys(descriptor.extension, node.data.type, 'label'), {
            defaultValue: descriptor.label,
          })}
        </strong>
        <code>{node.id}</code>
      </div>

      {descriptor.extension && (
        <p className="hint hint--strong">
          {t('inspector.fromExtension', { extension: extensionName })}
        </p>
      )}

      {descriptor.kind === 'dsp' && (
        <DomainControl
          nodeId={node.id}
          domain={node.data.domain}
          realtimeSafe={descriptor.realtimeSafe}
          recommendedColdBlock={descriptor.recommendedColdBlock}
        />
      )}

      {extension && extensionContext && (
        <ExtensionComponent
          extension={extension}
          surface="node-inspector"
          nodeType={node.data.type}
          context={extensionContext}
        />
      )}

      {descriptor.params.map((param) =>
        param.type === 'device' || param.type === 'text' || param.type === 'path' ? (
          <TargetControl
            key={param.name}
            param={param}
            extensionId={descriptor.extension}
            nodeType={node.data.type}
            value={node.data.options[param.name] ?? ''}
            onChange={(v) => setOption(node.id, param.name, v)}
            onEditStart={beginHistoryGroup}
            onEditEnd={endHistoryGroup}
          />
        ) : (
          <ParamControl
            key={param.name}
            nodeId={node.id}
            extensionId={descriptor.extension}
            nodeType={node.data.type}
            param={param}
            value={node.data.params[param.name] ?? (typeof param.default === 'number' ? param.default : 0)}
            onChange={(v) => setParam(node.id, param.name, v)}
          />
        ),
      )}

      {descriptor.dynamicInputs && (
        <label className="field">
          <span className="field__name">
            {t('inspector.inputs')}
            <output>{counts.inputs}</output>
          </span>
          <input
            type="range"
            min={2}
            max={16}
            step={1}
            value={counts.inputs}
            onPointerDown={beginHistoryGroup}
            onPointerUp={endHistoryGroup}
            onKeyDown={beginHistoryGroup}
            onKeyUp={endHistoryGroup}
            onChange={(e) => setPortCount(node.id, 'inputs', Number(e.target.value))}
          />
        </label>
      )}

      {descriptor.dynamicOutputs && (
        <label className="field">
          <span className="field__name">
            {t('inspector.outputs')}
            <output>{counts.outputs}</output>
          </span>
          <input
            type="range"
            min={2}
            max={16}
            step={1}
            value={counts.outputs}
            onPointerDown={beginHistoryGroup}
            onPointerUp={endHistoryGroup}
            onKeyDown={beginHistoryGroup}
            onKeyUp={endHistoryGroup}
            onChange={(e) => setPortCount(node.id, 'outputs', Number(e.target.value))}
          />
        </label>
      )}
    </div>
  )
}
