import { useTranslation } from 'react-i18next'
import { useMe, useUpdatePreferences } from '../../api/queries'
import type { Locale } from '../../api/types'
import { LOCALES } from '../../i18n'
import { cn } from '../../lib/cn'

/** VI | EN toggle; a signed-in user's choice is saved to their profile. */
export function LanguageSwitch({ tone = 'light', className }: { tone?: 'light' | 'dark'; className?: string }) {
  const { i18n, t } = useTranslation()
  const me = useMe().data
  const update = useUpdatePreferences()

  const choose = (locale: Locale) => {
    void i18n.changeLanguage(locale)
    if (me && me.locale !== locale) update.mutate({ locale })
  }

  return (
    <div
      role="radiogroup"
      aria-label={t('topbar.language')}
      className={cn('inline-flex rounded-xl p-1', tone === 'light' ? 'bg-slate-100' : 'bg-white/10', className)}
    >
      {LOCALES.map((locale) => {
        const selected = i18n.language === locale
        return (
          <button
            key={locale}
            type="button"
            role="radio"
            aria-checked={selected}
            onClick={() => choose(locale)}
            className={cn(
              'h-8 min-w-11 rounded-lg px-2.5 text-xs font-bold uppercase tracking-wide transition-colors',
              selected
                ? tone === 'light' ? 'bg-surface text-brand-700 shadow-sm' : 'bg-white text-navy-900'
                : tone === 'light' ? 'text-slate-500 hover:text-slate-900' : 'text-white/70 hover:text-white',
            )}
          >
            {locale}
          </button>
        )
      })}
    </div>
  )
}
