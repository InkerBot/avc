# Extension editor internationalization

An extension editor module can add translations from its `activate(api)` function. AVC gives
every extension its own i18next namespace, so resource keys cannot overwrite AVC or another
extension.

```js
export function activate(api) {
  api.i18n.addResources({
    'zh-CN': {
      name: '示例效果',
      nodes: { tremolo: { label: '颤音' } },
      ui: { save: '保存' },
    },
    'en-US': {
      name: 'Example effects',
      nodes: { tremolo: { label: 'Tremolo' } },
      ui: { save: 'Save' },
    },
  })
}
```

AVC reads these conventional keys when it renders extension metadata:

- `name` and `description`
- `category.<category>`
- `settings.<key>.name`, `.description`, and `.values.<value>`
- `nodes.<local-type>.label`, `.ports.<port>`, and
  `.params.<param>.name|description|values.<value>`
- `presets.<manifest-name>`

Node keys use the local type (`tremolo`), without the extension id prefix
(`example.tremolo`). All other keys are free for the extension UI to organize.

Custom elements receive the same scoped API as `element.avcContext.i18n`. Use
`i18n.t(key, options)` for text and subscribe with `i18n.onLanguageChanged(listener)` when a
mounted element needs to rerender. The returned function removes the listener. The current
language is available as `i18n.language`; `i18n.supportedLanguages` lists the languages accepted
by `addResources`.

See [`example.cpp`](./example.cpp) for a complete extension that localizes both AVC-owned
metadata and its own custom elements.
