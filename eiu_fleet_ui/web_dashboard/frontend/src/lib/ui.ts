import { create } from 'zustand'

/** Shared panels of the shell: the robot and task drawers, the create-task dialog, the selected robot on the map. */
interface UiState {
  robot: string | null
  task: number | null
  createTask: { open: boolean; service: string | null }
  openRobot: (name: string | null) => void
  openTask: (id: number | null) => void
  openCreateTask: (service?: string | null) => void
  closeCreateTask: () => void
}

export const useUi = create<UiState>((set) => ({
  robot: null,
  task: null,
  createTask: { open: false, service: null },
  openRobot: (robot) => set({ robot }),
  openTask: (task) => set({ task }),
  openCreateTask: (service = null) => set({ createTask: { open: true, service } }),
  closeCreateTask: () => set({ createTask: { open: false, service: null } }),
}))
