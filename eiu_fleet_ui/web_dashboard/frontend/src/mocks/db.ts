// Demo database: one JSON document in localStorage, kept in memory and written after each change.
import type { DbData } from './schema'
import { DB_VERSION, seed } from './seed'

const STORAGE_KEY = 'eiu-demo-db'

let data: DbData = load()

function load(): DbData {
  try {
    const raw = localStorage.getItem(STORAGE_KEY)
    if (raw) {
      const parsed = JSON.parse(raw) as DbData
      if (parsed.version === DB_VERSION) return parsed
    }
  } catch {
    // Storage unavailable or unreadable: start from the seed.
  }
  return seed(Date.now())
}

export function db(): DbData {
  return data
}

export function save(): void {
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(data))
  } catch {
    // Storage unavailable: the data lives in memory for this page.
  }
}

/** Replace all data with a fresh seed, keeping the signed-in session. */
export function resetDb(now: number): void {
  const session = data.session
  data = seed(now)
  data.session = session
  save()
}

/** Follow writes of other tabs of the demo. */
export function watchOtherTabs(onChange: () => void): void {
  window.addEventListener('storage', (e) => {
    if (e.key !== STORAGE_KEY || !e.newValue) return
    try {
      const parsed = JSON.parse(e.newValue) as DbData
      if (parsed.version === DB_VERSION) {
        data = parsed
        onChange()
      }
    } catch {
      // Ignore a partial write.
    }
  })
}
