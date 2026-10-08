import { useId } from 'react'

/** Delivery robot illustration: white cabinet, dark visor with lit eyes, wheels. */
export function RobotIllustration({ className }: { className?: string }) {
  const id = useId()
  return (
    <svg viewBox="0 0 160 200" className={className} role="img" aria-hidden>
      <defs>
        <linearGradient id={`${id}-body`} x1="0" x2="1">
          <stop offset="0" stopColor="#ffffff" />
          <stop offset="0.7" stopColor="#eef2f8" />
          <stop offset="1" stopColor="#d5dce8" />
        </linearGradient>
        <linearGradient id={`${id}-visor`} x1="0" y1="0" x2="0" y2="1">
          <stop offset="0" stopColor="#1b2740" />
          <stop offset="1" stopColor="#0b1222" />
        </linearGradient>
        <radialGradient id={`${id}-glow`}>
          <stop offset="0" stopColor="#7cc4ff" />
          <stop offset="1" stopColor="#2f6bff" stopOpacity="0" />
        </radialGradient>
      </defs>
      <ellipse cx="80" cy="190" rx="62" ry="7" fill="#000" opacity="0.18" />
      <rect x="22" y="150" width="116" height="26" rx="10" fill="#1f2937" />
      <circle cx="44" cy="178" r="12" fill="#111827" />
      <circle cx="44" cy="178" r="5" fill="#4b5563" />
      <circle cx="116" cy="178" r="12" fill="#111827" />
      <circle cx="116" cy="178" r="5" fill="#4b5563" />
      <rect x="26" y="30" width="108" height="128" rx="20" fill={`url(#${id}-body)`} />
      <rect x="26" y="96" width="108" height="3" fill="#cbd5e1" />
      <rect x="40" y="108" width="80" height="34" rx="8" fill="#e2e8f0" />
      <rect x="70" y="122" width="20" height="4" rx="2" fill="#94a3b8" />
      <rect x="38" y="42" width="84" height="44" rx="14" fill={`url(#${id}-visor)`} />
      <circle cx="62" cy="64" r="11" fill={`url(#${id}-glow)`} />
      <circle cx="98" cy="64" r="11" fill={`url(#${id}-glow)`} />
      <rect x="55" y="59" width="14" height="10" rx="5" fill="#8fd0ff" />
      <rect x="91" y="59" width="14" height="10" rx="5" fill="#8fd0ff" />
      <rect x="30" y="148" width="100" height="4" rx="2" fill="#5a8eff" opacity="0.8" />
    </svg>
  )
}

/** Robot head used as the app logo. */
export function LogoMark({ className }: { className?: string }) {
  return (
    <svg viewBox="0 0 48 48" className={className} aria-hidden>
      <rect x="21" y="3" width="6" height="7" rx="3" fill="#dce8ff" />
      <circle cx="24" cy="4" r="3" fill="#5a8eff" />
      <rect x="6" y="10" width="36" height="30" rx="10" fill="#ffffff" />
      <rect x="10" y="15" width="28" height="16" rx="6" fill="#0e2a5e" />
      <rect x="15" y="20" width="6" height="5" rx="2.5" fill="#8fd0ff" />
      <rect x="27" y="20" width="6" height="5" rx="2.5" fill="#8fd0ff" />
      <rect x="2" y="20" width="4" height="10" rx="2" fill="#bcd3ff" />
      <rect x="42" y="20" width="4" height="10" rx="2" fill="#bcd3ff" />
      <rect x="14" y="40" width="20" height="5" rx="2.5" fill="#bcd3ff" />
    </svg>
  )
}

/** Line-art campus skyline for the sidebar footer. */
export function SkylineArt({ className }: { className?: string }) {
  return (
    <svg viewBox="0 0 240 120" className={className} fill="none" stroke="currentColor" strokeWidth="1" aria-hidden>
      <path d="M0 118h240" />
      <path d="M8 118V70h26v48M14 78h4m6 0h4M14 88h4m6 0h4M14 98h4m6 0h4" />
      <path d="M40 118V40h34v78M46 50h6m10 0h6M46 62h6m10 0h6M46 74h6m10 0h6M46 86h6m10 0h6M46 98h6m10 0h6" />
      <path d="M57 40V28h8v12" />
      <path d="M82 118V60l22-14 22 14v58M92 70h6m12 0h6M92 84h6m12 0h6M92 98h6m12 0h6" />
      <path d="M134 118V52h30v66M140 60h5m8 0h5M140 72h5m8 0h5M140 84h5m8 0h5M140 96h5m8 0h5" />
      <path d="M172 118V80h40v38M180 90h6m10 0h6M180 102h6m10 0h6" />
      <path d="M218 118V96m-6 0a6 6 0 1 1 12 0a6 6 0 1 1-12 0" />
      <path d="M226 118v-8m-4 0a4 4 0 1 1 8 0a4 4 0 1 1-8 0" />
    </svg>
  )
}
