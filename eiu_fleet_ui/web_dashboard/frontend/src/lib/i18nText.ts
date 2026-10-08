import { useCallback } from 'react'
import { useTranslation } from 'react-i18next'
import type { I18nText, Locale } from '../api/types'

export function pickText(text: I18nText | string | number, locale: Locale): string {
  if (typeof text !== 'object') return String(text)
  return text[locale] ?? text.en ?? Object.values(text)[0] ?? ''
}

/** Resolves server text stored per language into the current language. */
export function useLocalize() {
  const { i18n } = useTranslation()
  const locale = (i18n.language === 'en' ? 'en' : 'vi') as Locale
  return useCallback((text: I18nText | string | number) => pickText(text, locale), [locale])
}
