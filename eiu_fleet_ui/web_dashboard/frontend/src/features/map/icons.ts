import L from 'leaflet'
import { blueprint } from './blueprint'

const pinSvg = (fill: string) => `
<svg viewBox="0 0 32 40" width="32" height="40" xmlns="http://www.w3.org/2000/svg">
  <path d="M16 39s13-13.4 13-23.5C29 7.5 23.2 2 16 2S3 7.5 3 15.5C3 25.6 16 39 16 39z" fill="${fill}" stroke="#fff" stroke-width="2.5"/>
  <circle cx="16" cy="15.5" r="5.5" fill="#fff"/>
</svg>`

const robotSvg = `
<svg viewBox="0 0 40 40" width="40" height="40" xmlns="http://www.w3.org/2000/svg">
  <rect x="5" y="5" width="30" height="30" rx="9" fill="#ffffff" stroke="#1a5cf5" stroke-width="2"/>
  <rect x="9" y="10" width="22" height="12" rx="5" fill="#0e2a5e"/>
  <rect x="12.5" y="14" width="5" height="4" rx="2" fill="#8fd0ff"/>
  <rect x="22.5" y="14" width="5" height="4" rx="2" fill="#8fd0ff"/>
  <rect x="11" y="26" width="18" height="3" rx="1.5" fill="#5a8eff"/>
</svg>`

export const pickupIcon = L.divIcon({ className: 'map-pin', html: pinSvg('#1a5cf5'), iconSize: [32, 40], iconAnchor: [16, 39], tooltipAnchor: [0, -34] })
export const dropoffIcon = L.divIcon({ className: 'map-pin', html: pinSvg('#10b981'), iconSize: [32, 40], iconAnchor: [16, 39], tooltipAnchor: [0, -34] })
export const dotIcon = L.divIcon({ className: '', html: '<div class="map-dot" style="width:10px;height:10px"></div>', iconSize: [10, 10], iconAnchor: [5, 5], tooltipAnchor: [0, -4] })
export const robotIcon = L.divIcon({
  className: '',
  html: `<div class="robot-marker"><div class="robot-marker__pulse"></div>${robotSvg}</div>`,
  iconSize: [40, 40],
  iconAnchor: [20, 20],
})

/**
 * Robot of the live map: a rounded tile in the color of its status with the icon of its service; the selected
 * robot gets a ring and the pulse.
 */
export function statusRobotIcon(color: string, serviceSvg: string, selected: boolean): L.DivIcon {
  const ring = selected ? '<div class="robot-marker__pulse"></div>' : ''
  return L.divIcon({
    className: '',
    html: `<div class="robot-marker${selected ? ' robot-marker--selected' : ''}">${ring}
      <svg viewBox="0 0 40 40" width="40" height="40" xmlns="http://www.w3.org/2000/svg">
        <rect x="4" y="4" width="32" height="32" rx="10" fill="${color}" stroke="#ffffff" stroke-width="${selected ? 3 : 2}"/>
      </svg>
      <span class="robot-marker__icon">${serviceSvg}</span></div>`,
    iconSize: [40, 40],
    iconAnchor: [20, 20],
    tooltipAnchor: [18, 0],
  })
}

/** Charging station of the nav graph: a small green tile with a bolt. */
export const chargerIcon = L.divIcon({
  className: 'map-pin',
  html: `<svg viewBox="0 0 24 24" width="22" height="22" xmlns="http://www.w3.org/2000/svg">
    <rect x="1.5" y="1.5" width="21" height="21" rx="6" fill="#ffffff" stroke="#5b8f45" stroke-width="2"/>
    <path d="M13 5 7.5 13H12l-1 6 5.5-8H12z" fill="#5b8f45"/></svg>`,
  iconSize: [22, 22],
  iconAnchor: [11, 11],
  tooltipAnchor: [0, -12],
})

const pinPath = 'M16 39s13-13.4 13-23.5C29 7.5 23.2 2 16 2S3 7.5 3 15.5C3 25.6 16 39 16 39z'

/** Pickup and destination pins of the blueprint map: colored pin with a dark outline. */
export function blueprintPin(fill: string): L.DivIcon {
  return L.divIcon({
    className: 'map-pin',
    html: `<svg viewBox="0 0 32 40" width="28" height="35" xmlns="http://www.w3.org/2000/svg">
      <path d="${pinPath}" fill="${fill}" stroke="${blueprint.routeCasing}" stroke-width="2.5"/><circle cx="16" cy="15.5" r="5" fill="${blueprint.routeCasing}"/></svg>`,
    iconSize: [28, 35],
    iconAnchor: [14, 34],
    tooltipAnchor: [0, -30],
  })
}

export interface BlueprintRobot {
  ring: string
  serviceSvg: string
  selected: boolean
  /** Small amber dot: the robot has a warning. */
  warning: boolean
  /** Slow pulse of the ring: the robot is in error and needs attention. */
  alert: boolean
}

/**
 * Robot of the blueprint map: dark round marker, the service as its icon, the status as its outer ring; selection
 * adds a white ring outside the status ring. A robot is round, a charging station square, so they never look alike.
 */
export function blueprintRobotIcon({ ring, serviceSvg, selected, warning, alert }: BlueprintRobot): L.DivIcon {
  const classes = ['bp-robot', selected && 'bp-robot--selected', alert && 'bp-robot--alert'].filter(Boolean).join(' ')
  return L.divIcon({
    className: '',
    html: `<div class="${classes}" style="--ring:${ring}">
      <svg viewBox="0 0 44 44" width="44" height="44" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">
        ${selected ? '<circle cx="22" cy="22" r="20.5" fill="none" stroke="#ffffff" stroke-width="2"/>' : ''}
        <circle class="bp-robot__ring" cx="22" cy="22" r="16.5" fill="${blueprint.markerFill}" stroke="${ring}" stroke-width="3"/>
      </svg>
      <span class="bp-robot__icon">${serviceSvg}</span>
      ${warning ? '<span class="bp-robot__warn"></span>' : ''}
    </div>`,
    iconSize: [44, 44],
    iconAnchor: [22, 22],
    tooltipAnchor: [20, 0],
  })
}

/** Charging station of the blueprint map: square facility tile with a bolt, green infrastructure styling. */
export const blueprintChargerIcon = L.divIcon({
  className: '',
  html: `<svg viewBox="0 0 24 24" width="22" height="22" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">
    <rect x="1.5" y="1.5" width="21" height="21" rx="4" fill="${blueprint.chargerFill}" stroke="${blueprint.chargerBorder}" stroke-width="1.5"/>
    <path d="M13 5 7.5 13H12l-1 6 5.5-8H12z" fill="#84cc16"/></svg>`,
  iconSize: [22, 22],
  iconAnchor: [11, 11],
  tooltipAnchor: [0, -12],
})
