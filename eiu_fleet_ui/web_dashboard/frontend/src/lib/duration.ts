/** Seconds as a short duration: "42 s", "5 min", "3.2 h", "2.1 d". */
export function duration(s: number): string {
  if (s < 60) return `${Math.round(s)} s`
  if (s < 3600) return `${Math.round(s / 60)} min`
  if (s < 86_400) return `${(s / 3600).toFixed(1)} h`
  return `${(s / 86_400).toFixed(1)} d`
}
