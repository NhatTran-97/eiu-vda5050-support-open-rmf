import { Check, Monitor, Moon, Sun, type LucideIcon } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { IconButton } from '../../components/ui/Button'
import { Menu, MenuItem } from '../../components/ui/Menu'
import { cn } from '../../lib/cn'
import { resolveTheme, useTheme, type ThemePref } from '../../lib/theme'

const OPTIONS: { value: ThemePref; icon: LucideIcon }[] = [
  { value: 'light', icon: Sun },
  { value: 'dark', icon: Moon },
  { value: 'system', icon: Monitor },
]

/** Light | Dark | System choice; `labels` shows the names next to the icons. */
export function ThemeSwitch({ labels, className }: { labels?: boolean; className?: string }) {
  const { t } = useTranslation()
  const { pref, setPref } = useTheme()
  return (
    <div role="radiogroup" aria-label={t('theme.label')} className={cn('inline-flex rounded-xl bg-slate-100 p-1', className)}>
      {OPTIONS.map(({ value, icon: Icon }) => (
        <button
          key={value}
          type="button"
          role="radio"
          aria-checked={pref === value}
          aria-label={t(`theme.${value}`)}
          title={t(`theme.${value}`)}
          onClick={() => setPref(value)}
          className={cn(
            'flex h-8 min-w-10 items-center justify-center gap-1.5 rounded-lg px-2.5 text-xs font-semibold transition-colors',
            pref === value ? 'bg-surface text-brand-700 shadow-sm' : 'text-slate-500 hover:text-slate-900',
          )}
        >
          <Icon className="size-4" aria-hidden />
          {labels && <span>{t(`theme.${value}`)}</span>}
        </button>
      ))}
    </div>
  )
}

/** Icon button that shows the theme in use and opens the three choices; `dark` tone for the navy top bar. */
export function ThemeMenu({ tone = 'dark' }: { tone?: 'light' | 'dark' }) {
  const { t } = useTranslation()
  const { pref, setPref } = useTheme()
  const Current = resolveTheme(pref) === 'dark' ? Moon : Sun
  return (
    <Menu
      className="min-w-48"
      trigger={({ toggle, open }) => (
        <IconButton tone={tone} label={t('theme.label')} onClick={toggle} aria-expanded={open}>
          <Current className="size-5" />
        </IconButton>
      )}
    >
      {(close) => OPTIONS.map(({ value, icon: Icon }) => (
        <MenuItem key={value} onClick={() => { setPref(value); close() }}>
          <Icon />
          <span className="flex-1">{t(`theme.${value}`)}</span>
          {pref === value && <Check className="size-4 text-brand-600" />}
        </MenuItem>
      ))}
    </Menu>
  )
}
