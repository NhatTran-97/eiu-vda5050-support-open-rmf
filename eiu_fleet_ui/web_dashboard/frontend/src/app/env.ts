/** Build-time switches from .env. */
export const USE_MOCKS = import.meta.env.VITE_USE_MOCKS === 'true'
export const API_BASE = '/api/v1'
