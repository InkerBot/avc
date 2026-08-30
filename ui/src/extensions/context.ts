import { useMemo } from 'react'

import { api, type Extension } from '../api'
import { useStore, type AvcNode } from '../store'
import { extensionRuntime, type ExtensionElementContext } from './runtime'

function settingsOf(extension: Extension): Record<string, string> {
  const values: Record<string, string> = {}
  extension.settings.forEach((setting) => {
    values[setting.key] = extension.values[setting.key] ?? setting.default
  })
  return values
}

export function useExtensionContext(
  extension: Extension | undefined,
  node?: Pick<AvcNode, 'id' | 'data'>,
): ExtensionElementContext | null {
  const saveSettings = useStore((state) => state.saveExtensionSettings)
  const setParam = useStore((state) => state.setParam)
  const setOption = useStore((state) => state.setOption)
  const nodeId = node?.id
  const nodeData = node?.data

  return useMemo(() => extension ? ({
    extension,
    call: (method, data = null) => api.callExtensionUi(extension.key, method, data),
    on: (event, listener) => extensionRuntime.on(extension.id, event, listener),
    settings: {
      get: () => settingsOf(extension),
      save: (values) => saveSettings(extension.key, values),
    },
    node: nodeId && nodeData ? {
      id: nodeId,
      type: nodeData.type,
      params: nodeData.params,
      options: nodeData.options,
      setParam: (name, value) => setParam(nodeId, name, value),
      setOption: (name, value) => setOption(nodeId, name, value),
    } : undefined,
  }) : null, [extension, nodeData, nodeId, saveSettings, setOption, setParam])
}
