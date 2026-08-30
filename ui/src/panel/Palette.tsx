import { useStore } from '../store'
import { useTranslation } from 'react-i18next'
import { colorOf } from '../theme'

export function Palette() {
  const palette = useStore((s) => s.palette)
  const addNode = useStore((s) => s.addNode)
  const { t } = useTranslation()

  const categories = [...new Set(palette.map((d) => d.category))]

  return (
    <div className="panel">
      <h2 className="panel__title">{t('palette.title')}</h2>
      {categories.map((category) => (
        <section key={category} className="palette__group">
          <h3 className="palette__category">
            {t(`category.${category}`, { defaultValue: category })}
          </h3>
          {palette
            .filter((d) => d.category === category)
            .map((d) => (
              <button
                key={d.type}
                className="palette__item"
                style={{ borderLeftColor: colorOf(d.category) }}
                onClick={() =>
                  addNode(d.type, { x: 260 + Math.random() * 160, y: 120 + Math.random() * 220 })
                }
                title={
                  d.extension
                    ? t('palette.fromExtension', {
                        extension: d.extension,
                        inputs: d.inputs.length,
                        outputs: d.outputs.length,
                      })
                    : t('palette.portCount', {
                        inputs: d.inputs.length,
                        outputs: d.outputs.length,
                      })
                }
              >
                <span>
                  {t(`nodes.${d.type}.label`, { defaultValue: d.label })}
                  {d.extension && <span className="palette__from">{d.extension}</span>}
                </span>
                <span className="palette__ports">
                  {d.inputs.length}→{d.outputs.length}
                </span>
              </button>
            ))}
        </section>
      ))}
    </div>
  )
}
