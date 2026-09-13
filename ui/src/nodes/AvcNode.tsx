import { Handle, NodeResizer, Position, type NodeProps } from '@xyflow/react'
import { useTranslation } from 'react-i18next'
import type { TFunction } from 'i18next'
import {
  useStore,
  portCounts,
  portNames,
  portTypes,
  AUDIO_PORT_TYPE,
  type AvcNode as AvcNodeType,
} from '../store'
import { colorOf, levelColor, levelFraction, portColorOf } from '../theme'
import { Scope } from './Scope'
import { TextOutput } from './TextOutput'
import { ExtensionComponent } from '../extensions/ExtensionComponent'
import { useExtensionContext } from '../extensions/context'
import { extensionNodeType, extensionTranslationKey } from '../i18n'

function portLabel(name: string, t: TFunction, nodeType: string, extensionId: string): string {
  if (extensionId) {
    const translated = t(
      extensionTranslationKey(
        extensionId,
        `nodes.${extensionNodeType(extensionId, nodeType)}.ports.${name}`,
      ),
      { defaultValue: '' },
    )
    if (translated) return translated
  }
  const m = /^(in|out)_(\d+)$/.exec(name)
  if (m) return t(m[1] === 'in' ? 'node.inN' : 'node.outN', { n: m[2] })
  if (name === 'in' || name === 'out') return t(`node.${name}`)
  return name
}

export function AvcNode({ id, data, selected }: NodeProps<AvcNodeType>) {
  const descriptor = useStore((s) => s.descriptors[data.type])
  const extension = useStore((s) => s.extensions.find((entry) => entry.id === descriptor?.extension))
  const devices = useStore((s) => s.audioDevices)
  const meter = useStore((s) => s.telemetry?.meters?.[id])
  const latencyMs = useStore((s) => s.telemetry?.nodeLatencyMs?.[id])
  const costUs = useStore((s) => s.telemetry?.nodeCost?.[id])
  const status = useStore((s) => s.telemetry?.nodeStatus?.[id])
  const scope = useStore((s) => s.scopes[id])
  const text = useStore((s) => {
    const conversation = s.texts[id]
    if (!conversation) return ''
    return [...conversation.history, ...(conversation.draft ? [conversation.draft] : [])]
      .map((message) => message.text)
      .join('\n')
  })
  const scopeBandsHz = useStore((s) => s.telemetry?.scopeBandsHz)
  const coldMs = useStore((s) =>
    data.domain ? s.telemetry?.domains?.find((d) => d.name === data.domain)?.latencyMs : undefined,
  )
  const { t } = useTranslation()
  const extensionContext = useExtensionContext(extension, { id, data })

  if (!descriptor) {
    return <div className="node node--unknown">{t('node.unknown', { type: data.type })}</div>
  }

  const counts = portCounts(descriptor, data)
  const inputs = portNames(descriptor.inputs, counts.inputs, 'in')
  const outputs = portNames(descriptor.outputs, counts.outputs, 'out')
  const inputTypes = portTypes(descriptor.inputs, counts.inputs)
  const outputTypes = portTypes(descriptor.outputs, counts.outputs)
  const accent = colorOf(descriptor.category)
  const label = t(
    descriptor.extension
      ? [
          extensionTranslationKey(
            descriptor.extension,
            `nodes.${extensionNodeType(descriptor.extension, data.type)}.label`,
          ),
          `nodes.${data.type}.label`,
        ]
      : `nodes.${data.type}.label`,
    { defaultValue: descriptor.label },
  )

  const targetParam =
    descriptor.kind === 'dsp'
      ? undefined
      : descriptor.params.find((p) => p.type === 'device' || p.type === 'text')
  const target = targetParam ? (data.options[targetParam.name] ?? '') : ''
  const device = devices.find((d) => d.name === target)
  const unavailable = Boolean(
    targetParam?.type === 'device' && target && !target.startsWith('@') && !device,
  )
  const shown = device
    ? device.mediaClass.startsWith('Stream/')
      ? device.application || device.name
      : device.description || device.name
    : target

  return (
    <div
      className={`node${data.type === 'text' ? ' node--text' : ''}${selected ? ' node--selected' : ''}${data.domain ? ' node--cold' : ''}${unavailable ? ' node--unavailable' : ''}`}
      style={{ borderTopColor: unavailable ? 'var(--accent)' : accent }}
    >
      <NodeResizer
        isVisible={selected}
        minWidth={data.type === 'text' ? 280 : 168}
        minHeight={64}
        color={accent}
        handleClassName="node__resize-handle"
        lineClassName="node__resize-line"
      />
      <div className="node__head">
        <span className="node__label">{label}</span>
        {status && status.state !== 'ready' && (
          <span className={`node__status node__status--${status.state}`} title={status.message}>
            {t(`node.state.${status.state}`)}
          </span>
        )}
        {data.domain && (
          <span
            className="node__cold"
            title={t('node.coldTitle', {
              name: data.domain,
              ms: (coldMs ?? 0).toFixed(1),
            })}
          >
            {data.domain}
          </span>
        )}
        <span className="node__id">{id}</span>
      </div>

      {targetParam && (
        <div
          className={`node__endpoint${target ? '' : ' node__endpoint--unset'}${unavailable ? ' node__endpoint--gone' : ''}`}
          title={unavailable ? t('node.objectMissing') : undefined}
        >
          <span className="node__endpoint-index" style={{ background: accent }}>
            {descriptor.kind === 'capture' || descriptor.kind === 'virtual_speaker' ? '↦' : '↤'}
          </span>
          <span className="node__endpoint-name" title={target}>
            {shown || t('node.nothingSelected')}
          </span>
          {unavailable && <span className="node__endpoint-error">{t('node.objectMissing')}</span>}
        </div>
      )}

      {data.params.exclusive >= 0.5 && (
        <div className="node__claim">{t('node.exclusive')}</div>
      )}

      {(latencyMs !== undefined || costUs !== undefined) && (
        <div className="node__cost">
          {latencyMs !== undefined && (
            <span className="node__cost-item node__cost-item--late">
              {t('node.latency', { ms: latencyMs.toFixed(1) })}
            </span>
          )}
          {costUs !== undefined && (
            <span className="node__cost-item">{t('node.cost', { us: costUs.toFixed(1) })}</span>
          )}
        </div>
      )}

      {scope && <Scope wave={scope.wave} bands={scope.bands} bandsHz={scopeBandsHz ?? []} />}

      {data.type === 'text' && <TextOutput text={text} />}

      {extension && extensionContext && (
        <ExtensionComponent
          extension={extension}
          surface="node-body"
          nodeType={data.type}
          context={extensionContext}
        />
      )}

      {meter && (
        <div className="meter" title={t('node.peak', { db: meter.peak.toFixed(1) })}>
          <div
            className="meter__fill"
            style={{
              width: `${levelFraction(meter.peak) * 100}%`,
              background: levelColor(meter.peak),
            }}
          />
          <span className="meter__value">
            {meter.peak <= -119 ? '—' : `${meter.peak.toFixed(0)} dB`}
          </span>
        </div>
      )}

      <div className="node__ports">
        <div className="node__column">
          {inputs.map((name, i) => (
            <div className="port" key={name}>
              <Handle
                type="target"
                position={Position.Left}
                id={`in:${name}`}
                className={inputTypes[i] === AUDIO_PORT_TYPE ? undefined : 'handle--typed'}
                title={inputTypes[i]}
                style={{
                  top: 0,
                  position: 'relative',
                  transform: 'none',
                  background: portColorOf(inputTypes[i]),
                }}
              />
              <span className="port__name">
                {portLabel(name, t, data.type, descriptor.extension)}
              </span>
              {inputTypes[i] !== AUDIO_PORT_TYPE && (
                <span className="port__type">{inputTypes[i]}</span>
              )}
            </div>
          ))}
        </div>
        <div className="node__column node__column--out">
          {outputs.map((name, i) => (
            <div className="port port--out" key={name}>
              {outputTypes[i] !== AUDIO_PORT_TYPE && (
                <span className="port__type">{outputTypes[i]}</span>
              )}
              <span className="port__name">
                {portLabel(name, t, data.type, descriptor.extension)}
              </span>
              <Handle
                type="source"
                position={Position.Right}
                id={`out:${name}`}
                className={outputTypes[i] === AUDIO_PORT_TYPE ? undefined : 'handle--typed'}
                title={outputTypes[i]}
                style={{
                  top: 0,
                  position: 'relative',
                  transform: 'none',
                  background: portColorOf(outputTypes[i]),
                }}
              />
            </div>
          ))}
        </div>
      </div>
    </div>
  )
}
