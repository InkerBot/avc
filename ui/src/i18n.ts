import i18n, { type ResourceKey, type TOptions } from 'i18next'
import { initReactI18next } from 'react-i18next'
import { enUS } from './locales/en-US'
import { zhCN } from './locales/zh-CN'

export const supportedLanguages = {
  'zh-CN': '简体中文',
  'en-US': 'English',
} as const

export type SupportedLanguage = keyof typeof supportedLanguages

export type ExtensionTranslationResources = Partial<Record<SupportedLanguage, ResourceKey>>

export interface ExtensionI18n {
  readonly language: SupportedLanguage
  readonly supportedLanguages: readonly SupportedLanguage[]
  t(key: string, options?: TOptions): string
  addResources(resources: ExtensionTranslationResources): void
  onLanguageChanged(listener: (language: SupportedLanguage) => void): () => void
}

const LANGUAGE_STORAGE_KEY = 'avc.language'

export function resolveSupportedLanguage(language?: string | null): SupportedLanguage {
  const normalized = language?.toLowerCase()
  if (normalized?.startsWith('zh')) return 'zh-CN'
  if (normalized?.startsWith('en')) return 'en-US'
  return 'zh-CN'
}

function detectLanguage(): SupportedLanguage {
  try {
    const stored = window.localStorage.getItem(LANGUAGE_STORAGE_KEY)
    if (stored && Object.hasOwn(supportedLanguages, stored)) {
      return stored as SupportedLanguage
    }
  } catch {
    // Storage can be unavailable in hardened or embedded browser contexts.
  }

  for (const language of navigator.languages ?? [navigator.language]) {
    if (/^(zh|en)(-|$)/i.test(language)) return resolveSupportedLanguage(language)
  }
  return 'zh-CN'
}

function syncDocumentLanguage(language: string) {
  const supported = resolveSupportedLanguage(language)
  document.documentElement.lang = supported
  try {
    window.localStorage.setItem(LANGUAGE_STORAGE_KEY, supported)
  } catch {
    // The selected language still applies to the current session.
  }
}

export function extensionI18nNamespace(extensionId: string): string {
  return `extension-${extensionId}`
}

export function extensionNodeType(extensionId: string, nodeType: string): string {
  const prefix = `${extensionId}.`
  return nodeType.startsWith(prefix) ? nodeType.slice(prefix.length) : nodeType
}

export function extensionTranslationKey(extensionId: string, key: string): string {
  return `${extensionI18nNamespace(extensionId)}:${key}`
}

const extensionI18nApis = new Map<string, ExtensionI18n>()

export function extensionI18n(extensionId: string): ExtensionI18n {
  const current = extensionI18nApis.get(extensionId)
  if (current) return current

  const namespace = extensionI18nNamespace(extensionId)
  const api = Object.freeze({
    get language() {
      return resolveSupportedLanguage(i18n.resolvedLanguage ?? i18n.language)
    },
    supportedLanguages: Object.freeze(
      Object.keys(supportedLanguages) as SupportedLanguage[],
    ) as readonly SupportedLanguage[],
    t(key: string, options: TOptions = {}) {
      return String(i18n.t(key, { ...options, ns: namespace }))
    },
    addResources(resources: ExtensionTranslationResources) {
      for (const language of Object.keys(supportedLanguages) as SupportedLanguage[]) {
        const bundle = resources[language]
        if (bundle) i18n.addResourceBundle(language, namespace, bundle, true, true)
      }
    },
    onLanguageChanged(listener: (language: SupportedLanguage) => void) {
      const wrapped = (language: string) => listener(resolveSupportedLanguage(language))
      i18n.on('languageChanged', wrapped)
      return () => i18n.off('languageChanged', wrapped)
    },
  }) satisfies ExtensionI18n
  extensionI18nApis.set(extensionId, api)
  return api
}

export function removeExtensionI18n(extensionId: string): void {
  const namespace = extensionI18nNamespace(extensionId)
  for (const language of Object.keys(supportedLanguages) as SupportedLanguage[]) {
    i18n.removeResourceBundle(language, namespace)
  }
  extensionI18nApis.delete(extensionId)
}

i18n.on('languageChanged', syncDocumentLanguage)

void i18n.use(initReactI18next).init({
  resources: {
    'zh-CN': { translation: zhCN },
    'en-US': { translation: enUS },
  },
  lng: detectLanguage(),
  fallbackLng: 'en-US',
  supportedLngs: Object.keys(supportedLanguages),
  interpolation: { escapeValue: false },
  react: { bindI18nStore: 'added removed' },
})

export default i18n
