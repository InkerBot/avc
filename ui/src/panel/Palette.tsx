import { useEffect, useRef, useState } from 'react'
import { useStore } from '../store'
import { useTranslation } from 'react-i18next'
import { colorOf } from '../theme'
import { extensionNodeType, extensionTranslationKey } from '../i18n'
import { useReactFlow } from '@xyflow/react'

function openPosition(
  initial: { x: number; y: number },
  nodes: { position: { x: number; y: number } }[],
) {
  for (let step = 0; step < 24; step += 1) {
    const column = step % 4
    const row = Math.floor(step / 4)
    const candidate = {
      x: initial.x + (column - 1.5) * 42,
      y: initial.y + row * 34,
    }
    const occupied = nodes.some((node) =>
      Math.abs(node.position.x - candidate.x) < 150
      && Math.abs(node.position.y - candidate.y) < 96,
    )
    if (!occupied) return candidate
  }
  return initial
}

export function Palette() {
  const palette = useStore((s) => s.palette)
  const extensions = useStore((s) => s.extensions)
  const addNode = useStore((s) => s.addNode)
  const nodes = useStore((s) => s.nodes)
  const busy = useStore((s) => s.graphBusy !== null)
  const { screenToFlowPosition } = useReactFlow()
  const { t } = useTranslation()
  const [query, setQuery] = useState('')
  const search = useRef<HTMLInputElement>(null)

  const labelOf = (descriptor: (typeof palette)[number]) => t(
    descriptor.extension
      ? [
          extensionTranslationKey(
            descriptor.extension,
            `nodes.${extensionNodeType(descriptor.extension, descriptor.type)}.label`,
          ),
          `nodes.${descriptor.type}.label`,
        ]
      : `nodes.${descriptor.type}.label`,
    { defaultValue: descriptor.label },
  )
  const categoryOf = (category: string) => t(
    [
      `category.${category}`,
      ...palette
        .filter((descriptor) => descriptor.category === category && descriptor.extension)
        .map((descriptor) => extensionTranslationKey(descriptor.extension, `category.${category}`)),
    ],
    { defaultValue: category },
  )
  const needle = query.trim().toLocaleLowerCase()
  const filtered = needle
    ? palette.filter((descriptor) => [
        labelOf(descriptor),
        descriptor.label,
        descriptor.type,
        categoryOf(descriptor.category),
      ].some((value) => value.toLocaleLowerCase().includes(needle)))
    : palette
  const categories = [...new Set(filtered.map((d) => d.category))]

  useEffect(() => {
    const focusSearch = (event: KeyboardEvent) => {
      const target = event.target as HTMLElement | null
      if (
        event.key !== '/'
        || event.ctrlKey
        || event.metaKey
        || event.altKey
        || document.querySelector('.dialog-backdrop')
        || target?.closest('input, textarea, select, [contenteditable="true"]')
        || !search.current
        || search.current.offsetParent === null
      ) return
      event.preventDefault()
      search.current.focus()
    }
    window.addEventListener('keydown', focusSearch)
    return () => window.removeEventListener('keydown', focusSearch)
  }, [])

  return (
    <div className="panel">
      <div className="palette__head">
        <h2 className="panel__title">{t('palette.title')}</h2>
        <kbd aria-hidden="true">/</kbd>
      </div>
      <input
        ref={search}
        className="palette__search"
        type="search"
        value={query}
        placeholder={t('palette.search')}
        aria-label={t('palette.search')}
        aria-keyshortcuts="/"
        onChange={(event) => setQuery(event.target.value)}
        onKeyDown={(event) => {
          if (event.key === 'Escape' && query) {
            event.stopPropagation()
            setQuery('')
          }
        }}
      />
      {categories.map((category) => (
        <section key={category} className="palette__group">
          <h3 className="palette__category">{categoryOf(category)}</h3>
          {filtered
            .filter((d) => d.category === category)
            .map((d) => (
              <button
                key={d.type}
                className="palette__item"
                style={{ borderLeftColor: colorOf(d.category) }}
                draggable
                disabled={busy}
                title={t('palette.addHint')}
                onDragStart={(event) => {
                  event.dataTransfer.setData('application/x-avc-node', d.type)
                  event.dataTransfer.effectAllowed = 'copy'
                }}
                onClick={() => {
                  const canvas = document.querySelector<HTMLElement>('.canvas')
                  const bounds = canvas?.getBoundingClientRect()
                  const center = screenToFlowPosition({
                    x: bounds ? bounds.left + bounds.width / 2 : window.innerWidth / 2,
                    y: bounds ? bounds.top + bounds.height / 2 : window.innerHeight / 2,
                  })
                  addNode(d.type, openPosition(center, nodes))
                }}
              >
                <span>
                  {labelOf(d)}
                  {d.extension && (
                    <span className="palette__from">
                      {t(extensionTranslationKey(d.extension, 'name'), {
                        defaultValue:
                          extensions.find((extension) => extension.id === d.extension)?.name
                          || d.extension,
                      })}
                    </span>
                  )}
                </span>
                <span className="palette__ports">
                  {d.inputs.length}→{d.outputs.length}
                </span>
              </button>
            ))}
        </section>
      ))}
      {categories.length === 0 && (
        <p className="hint palette__empty" role="status">{t('palette.noResults')}</p>
      )}
    </div>
  )
}
