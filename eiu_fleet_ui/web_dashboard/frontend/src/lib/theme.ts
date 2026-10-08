import { create } from 'zustand'

/** The user's choice; `system` follows the operating system setting. */
export type ThemePref = 'light' | 'dark' | 'system'

// Read by the inline script of index.html before the first paint.
const STORAGE_KEY = 'eiu-theme'
const DARK_QUERY = '(prefers-color-scheme: dark)'
const META_COLOR = { light: '#0a1f47', dark: '#050f26' }

function stored(): ThemePref {
  try {
    const value = localStorage.getItem(STORAGE_KEY)
    return value === 'light' || value === 'dark' ? value : 'system'
  } catch {
    return 'system'
  }
}

function systemDark(): boolean {
  return typeof matchMedia === 'function' && matchMedia(DARK_QUERY).matches
}

export function resolveTheme(pref: ThemePref): 'light' | 'dark' {
  return pref === 'system' ? (systemDark() ? 'dark' : 'light') : pref
}

function apply(pref: ThemePref): void {
  const theme = resolveTheme(pref)
  document.documentElement.dataset.theme = theme
  document.querySelector('meta[name="theme-color"]')?.setAttribute('content', META_COLOR[theme])
}

interface ThemeState {
  pref: ThemePref
  setPref: (pref: ThemePref) => void
}

export const useTheme = create<ThemeState>((set) => ({
  pref: stored(),
  setPref: (pref) => {
    try {
      if (pref === 'system') localStorage.removeItem(STORAGE_KEY)
      else localStorage.setItem(STORAGE_KEY, pref)
    } catch {
      // Storage unavailable: the choice lasts for this page.
    }
    apply(pref)
    set({ pref })
  },
}))

/** Applies the stored choice and follows system changes while the choice is `system`. */
export function startTheme(): void {
  apply(useTheme.getState().pref)
  if (typeof matchMedia !== 'function') return
  matchMedia(DARK_QUERY).addEventListener('change', () => {
    if (useTheme.getState().pref === 'system') apply('system')
  })
}
