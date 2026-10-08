// Blue-gray technical blueprint of the live operations map: shared map tokens (the CSS side is the --bp-* variables
// of styles/index.css). Robot status colors are the shared
// `robotStatusColor` of domain/status.tsx; a robot's service is its icon, never its color.
export const blueprint = {
  background: '#283d52',
  surface: '#30475e',
  wall: '#b8c6d3',
  geometry: '#8597aa',
  poiDot: '#e2eaf2',
  markerFill: '#1d2e42',
  routeActive: '#3b82f6',
  routeHover: '#60a5fa',
  routeCompleted: '#7590ab',
  routeRemaining: 'rgba(59, 130, 246, 0.45)',
  routeCasing: '#0e1a2a',
  destination: '#22c55e',
  pickup: '#3b82f6',
  warning: '#f59e0b',
  critical: '#ef4444',
  selectedRing: '#ffffff',
  hoverRing: '#93c5fd',
  chargerFill: '#1e3326',
  chargerIcon: '#84cc16',
  chargerBorder: '#4d7c0f',
}

/** Inner padding between the floor plan and the panel edge, in pixels. */
export const FIT_PADDING_PX = 24
/** Occupancy pixel values: below `wallMax` is a wall, above `freeMin` free space, between is unknown space;
 * `trim` is the share of floor pixels dropped at each side when measuring the floor's extent. */
export const OCCUPANCY = { wallMax: 120, freeMin: 235, trim: 0.003 }
