// Settings of the demo backend; the real backend reads the same values from its YAML config.
export const demoSettings = {
  timeZone: 'Asia/Ho_Chi_Minh',
  tickMs: 500,
  latencyMs: 120,
  robotSpeedMps: 0.25,
  loadingDwellS: 20,
  collectDwellS: 15,
  arrivingSoonS: 30,
  batteryDrainPerM: 0.08,
  maxActiveDeliveries: 3,
  maxPatrolStops: 8,
  maxPatrolRounds: 10,
  minPasswordLength: 8,
  support: {
    email: 'robot-support@eiu.edu.vn',
    phone: '0274 222 0347',
    hours: { vi: 'Thứ 2 – Thứ 6, 7:30 – 17:00', en: 'Mon – Fri, 7:30 AM – 5:00 PM' },
  },
}

export type DemoSettings = typeof demoSettings
