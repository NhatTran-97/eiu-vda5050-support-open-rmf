import type { ReactNode } from 'react'
import { cn } from '../../lib/cn'

export interface Column<T> {
  key: string
  header: ReactNode
  cell: (row: T) => ReactNode
  className?: string
}

/** Table that scrolls sideways on narrow screens; rows open on click when `onRow` is given. */
export function DataTable<T>({ columns, rows, rowKey, onRow, selected, empty, minWidth = 880, className }: {
  columns: Column<T>[]
  rows: T[]
  rowKey: (row: T) => string | number
  onRow?: (row: T) => void
  selected?: string | number | null
  empty?: ReactNode
  minWidth?: number
  className?: string
}) {
  if (rows.length === 0 && empty) return <>{empty}</>
  return (
    <div className={cn('relative overflow-x-auto', className)}>
      <table className="w-full text-sm" style={{ minWidth }}>
        <thead>
          <tr className="border-b border-slate-100">
            {columns.map((c) => (
              <th key={c.key} scope="col" className={cn('px-3 py-2.5 text-left text-xs font-semibold tracking-wide whitespace-nowrap text-slate-500 uppercase', c.className)}>
                {c.header}
              </th>
            ))}
          </tr>
        </thead>
        <tbody className="divide-y divide-slate-100">
          {rows.map((row) => {
            const key = rowKey(row)
            return (
              <tr
                key={key}
                onClick={onRow ? () => onRow(row) : undefined}
                className={cn(onRow && 'cursor-pointer transition-colors hover:bg-ops-subtle', selected === key && 'bg-brand-50/60')}
              >
                {columns.map((c) => <td key={c.key} className={cn('px-3 py-2.5 align-middle', c.className)}>{c.cell(row)}</td>)}
              </tr>
            )
          })}
        </tbody>
      </table>
    </div>
  )
}
