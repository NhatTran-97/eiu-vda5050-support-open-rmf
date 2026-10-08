import { describe, expect, it } from 'vitest'
import { fold, initials } from './text'

describe('fold', () => {
  it('removes Vietnamese diacritics and case', () => {
    expect(fold('Phòng 204')).toBe('phong 204')
    expect(fold('Trung tâm Sinh viên')).toBe('trung tam sinh vien')
    expect(fold('Đà Lạt')).toBe('da lat')
  })
})

describe('initials', () => {
  it('takes the first and last word', () => {
    expect(initials('Nguyễn Minh Anh')).toBe('NA')
    expect(initials('admin')).toBe('A')
  })
})
