import { Suspense } from 'react'
import { useTranslation } from 'react-i18next'
import { Outlet, ScrollRestoration } from 'react-router'
import { Spinner } from '../components/ui/States'
import { Toaster } from '../components/ui/Toaster'
import { RobotDetailsDrawer } from '../features/fleet/RobotDetailsDrawer'
import { CreateTaskDialog } from '../features/tasks/CreateTaskDialog'
import { TaskDetailsDrawer } from '../features/tasks/TaskDetailsDrawer'
import { BottomNav } from './BottomNav'
import { RmfBanner } from './RmfBanner'
import { Sidebar } from './Sidebar'
import { TopBar } from './TopBar'

/**
 * Page frame. lg and wider: fixed sidebar, top bar, content. Below lg: top bar with the brand, content, bottom tab bar.
 * The robot and task drawers and the create-task dialog live here, so every page can open them.
 * Every page uses the light operations workspace of the design brief (dark layers in the dark theme).
 */
export function AppShell() {
  const { t } = useTranslation()
  return (
    <div className="ops-bg min-h-dvh">
      <a href="#main" className="sr-only z-50 rounded-lg bg-surface px-4 py-2 font-semibold focus:not-sr-only focus:fixed focus:top-3 focus:left-3">
        {t('app.skipToContent')}
      </a>
      <Sidebar />
      <div className="flex min-h-dvh flex-col lg:pl-64">
        <TopBar light />
        <RmfBanner />
        <main id="main" className="mx-auto w-full max-w-[2560px] flex-1 px-4 pb-28 sm:px-6 lg:px-8 lg:pb-8">
          <Suspense fallback={<div className="flex justify-center py-24"><Spinner className="size-8 text-ops-blue" /></div>}>
            <Outlet />
          </Suspense>
        </main>
      </div>
      <BottomNav />
      <RobotDetailsDrawer />
      <TaskDetailsDrawer />
      <CreateTaskDialog />
      <Toaster />
      <ScrollRestoration />
    </div>
  )
}
