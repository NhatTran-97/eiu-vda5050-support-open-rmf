import i18n from 'i18next'
import { initReactI18next } from 'react-i18next'
import type { Locale } from '../api/types'
import en from './en.json'
import vi from './vi.json'

export const LOCALES: Locale[] = ['vi', 'en']
export const BCP47: Record<Locale, string> = { vi: 'vi-VN', en: 'en-US' }
const STORAGE_KEY = 'eiu.locale'

function storedLocale(): Locale | null {
  try {
    const value = localStorage.getItem(STORAGE_KEY)
    return LOCALES.includes(value as Locale) ? (value as Locale) : null
  } catch {
    return null
  }
}

void i18n.use(initReactI18next).init({
  resources: { vi: { translation: vi }, en: { translation: en } },
  lng: storedLocale() ?? 'vi',
  fallbackLng: 'en',
  interpolation: { escapeValue: false },
})

document.documentElement.lang = i18n.language
i18n.on('languageChanged', (lng) => {
  document.documentElement.lang = lng
  try {
    localStorage.setItem(STORAGE_KEY, lng)
  } catch {
    // Storage unavailable: the choice lasts for this page.
  }
})

export function currentLocale(): Locale {
  return LOCALES.includes(i18n.language as Locale) ? (i18n.language as Locale) : 'vi'
}

export default i18n
