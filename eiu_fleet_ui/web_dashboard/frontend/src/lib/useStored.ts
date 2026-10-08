import { useState } from 'react'

/** State kept in localStorage for this browser; falls back to `initial` when storage is unavailable or holds junk. */
export function useStored<T>(key: string, initial: T, valid: (value: unknown) => value is T): [T, (value: T) => void] {
  const [value, setValue] = useState<T>(() => {
    try {
      const raw = localStorage.getItem(key)
      const parsed: unknown = raw === null ? null : JSON.parse(raw)
      return valid(parsed) ? parsed : initial
    } catch {
      return initial
    }
  })
  const store = (next: T) => {
    setValue(next)
    try {
      localStorage.setItem(key, JSON.stringify(next))
    } catch {
      // Storage unavailable: the value lasts for this page.
    }
  }
  return [value, store]
}
