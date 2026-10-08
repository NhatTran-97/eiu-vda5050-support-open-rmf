import { useCallback } from 'react'
import { useTranslation } from 'react-i18next'
import { ApiError } from '../api/client'
import { useConfig } from '../api/queries'

/** Message for an API error code in the current language. */
export function useErrorText() {
  const { t, i18n } = useTranslation()
  const config = useConfig().data
  return useCallback((error: unknown) => {
    if (!(error instanceof ApiError)) return t('errors.generic')
    const message = error.message && error.message !== error.code ? error.message : ''
    // Authorization errors (SERVICE_NOT_ALLOWED, ...) carry a message in the user's language.
    if (/^[A-Z_]+$/.test(error.code) && message) return message
    // Task form errors name the field in the message.
    if (error.code.startsWith('task.') && message && i18n.exists(`errors.${error.code}`)) {
      return t(`errors.${error.code}`, { field: t(`taskForm.field.${message}`, { defaultValue: message }) })
    }
    if (i18n.exists(`errors.${error.code}`)) {
      const text = t(`errors.${error.code}`, { max: config?.maxActiveDeliveries })
      return message ? `${text} (${message})` : text
    }
    return message || t('errors.generic')
  }, [t, i18n, config?.maxActiveDeliveries])
}
