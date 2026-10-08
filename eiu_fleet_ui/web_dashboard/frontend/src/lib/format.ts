import { useMemo } from 'react'
import { useTranslation } from 'react-i18next'
import { useConfig } from '../api/queries'
import type { Locale } from '../api/types'
import { BCP47 } from '../i18n'

/** Date, time and distance formatting in the current language and the site's time zone. */
export function useFormat() {
  const { i18n, t } = useTranslation()
  const timeZone = useConfig().data?.timeZone
  const locale = BCP47[(i18n.language === 'en' ? 'en' : 'vi') as Locale]

  return useMemo(() => {
    const dateTime = new Intl.DateTimeFormat(locale, { timeZone, dateStyle: 'medium', timeStyle: 'short' })
    const time = new Intl.DateTimeFormat(locale, { timeZone, hour: 'numeric', minute: '2-digit' })
    const date = new Intl.DateTimeFormat(locale, { timeZone, day: 'numeric', month: 'short', year: 'numeric' })
    const shortDate = new Intl.DateTimeFormat(locale, { timeZone, day: 'numeric', month: 'short' })
    const dayKey = new Intl.DateTimeFormat('en-CA', { timeZone, year: 'numeric', month: '2-digit', day: '2-digit' })
    const number = new Intl.NumberFormat(locale, { maximumFractionDigits: 1 })

    return {
      dateTime: (ts: number) => dateTime.format(ts),
      time: (ts: number) => time.format(ts),
      date: (ts: number) => date.format(ts),
      shortDate: (ts: number) => shortDate.format(ts),
      dateRange: (a: number, b: number) => (date.format(a) === date.format(b) ? date.format(a) : date.formatRange(a, b)),
      /** Calendar day in the site's time zone, as YYYY-MM-DD. */
      dayKey: (ts: number) => dayKey.format(ts),
      distance: (m: number) => (m < 1000 ? `${Math.round(m)} m` : `${number.format(m / 1000)} km`),
      percent: (v: number) => `${Math.round(v)}%`,
      /** Time left until `at`, in minutes. */
      timeLeft: (at: number, now: number) => {
        const ms = at - now
        if (ms < 60_000) return t('time.lessThanMinute')
        return t('time.minutes', { count: Math.ceil(ms / 60_000) })
      },
    }
  }, [locale, timeZone, t])
}
