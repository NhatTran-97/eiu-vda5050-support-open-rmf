import { describe, expect, it } from 'vitest'
import en from './en.json'
import vi from './vi.json'

function keys(obj: object, prefix = ''): string[] {
  return Object.entries(obj).flatMap(([k, v]) =>
    v && typeof v === 'object' ? keys(v as object, `${prefix}${k}.`) : [`${prefix}${k}`])
}

function params(text: string): string[] {
  return [...text.matchAll(/{{(\w+)}}/g)].map((m) => m[1]).sort()
}

function lookup(obj: object, key: string): string {
  return key.split('.').reduce<unknown>((o, k) => (o as Record<string, unknown>)[k], obj) as string
}

describe('translations', () => {
  it('have the same keys in Vietnamese and English', () => {
    expect(keys(vi).sort()).toEqual(keys(en).sort())
  })

  it('use the same placeholders in both languages', () => {
    for (const key of keys(en)) expect(params(lookup(vi, key)), key).toEqual(params(lookup(en, key)))
  })
})
