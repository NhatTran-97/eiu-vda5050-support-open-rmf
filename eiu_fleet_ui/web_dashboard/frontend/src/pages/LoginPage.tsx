import { Eye, EyeOff, LogIn } from 'lucide-react'
import { useState, type FormEvent } from 'react'
import { useTranslation } from 'react-i18next'
import { Navigate, useNavigate, useSearchParams } from 'react-router'
import { useConfig, useLogin, useMe } from '../api/queries'
import eiuMark from '../assets/eiu-mark.png'
import campus from '../assets/login-campus.webp'
import { RobotIllustration } from '../assets/RobotArt'
import { Button, IconButton } from '../components/ui/Button'
import { Field, Input } from '../components/ui/Field'
import { safeNext } from '../features/auth/guards'
import { LanguageSwitch } from '../features/auth/LanguageSwitch'
import { ThemeMenu } from '../features/auth/ThemeSwitch'
import { useErrorText } from '../lib/errors'
import i18n from '../i18n'

export default function LoginPage() {
  const { t } = useTranslation()
  const navigate = useNavigate()
  const [params] = useSearchParams()
  const next = safeNext(params.get('next'))
  const me = useMe()
  const config = useConfig().data
  const login = useLogin()
  const errorText = useErrorText()
  const [email, setEmail] = useState('')
  const [password, setPassword] = useState('')
  const [showPassword, setShowPassword] = useState(false)

  if (me.data) return <Navigate to={next} replace />

  const submit = (e: FormEvent) => {
    e.preventDefault()
    login.mutate({ email, password }, {
      onSuccess: (user) => {
        void i18n.changeLanguage(user.locale)
        navigate(next, { replace: true })
      },
    })
  }

  return (
    <div className="ops-bg grid min-h-dvh grid-cols-1 lg:grid-cols-[minmax(0,54fr)_minmax(0,46fr)]">
      {/* Campus hero: the same photo in both themes, treated per theme (light: a soft white wash and navy text; dark: a
          navy overlay (40 %), slightly desaturated, and white text; slate/brand tokens are remapped for dark cards, so the copy uses
          white at an opacity). A scrim on the text side keeps the copy readable. On phones
          it is a compact banner above the form. */}
      <aside className="relative isolate flex min-h-60 flex-col justify-between gap-6 overflow-hidden p-6 sm:p-8 lg:min-h-0 lg:p-12">
        <img src={campus} alt="" aria-hidden className="absolute inset-0 -z-20 size-full object-cover dark:brightness-95 dark:saturate-[.85]" />
        <div aria-hidden className="absolute inset-0 -z-10 bg-white/20 dark:bg-navy-950/40" />
        <div aria-hidden className="absolute inset-0 -z-10 bg-linear-to-r from-white/80 via-white/45 to-transparent dark:from-navy-950/65 dark:via-navy-950/20" />
        {/* Phones: the copy spans the whole banner, so the wash is even across it. */}
        <div aria-hidden className="absolute inset-0 -z-10 bg-white/35 lg:hidden dark:bg-navy-950/25" />
        {/* Institution first (the EIU emblem and wordmark, cropped from the official logo, colors untouched; on the dark
            theme it sits on a light plate because no white variant exists), then the product name and its subtitle. */}
        <div className="flex items-center gap-3 sm:gap-4">
          <span className="shrink-0 rounded-lg dark:bg-white/90 dark:px-2 dark:py-1.5">
            <img src={eiuMark} alt="EIU · Eastern International University" className="h-8 w-auto sm:h-9 lg:h-10" />
          </span>
          <span aria-hidden className="h-9 w-px shrink-0 bg-eiu-navy/25 dark:bg-white/30" />
          <span className="min-w-0 leading-tight">
            <span className="block text-base font-extrabold tracking-tight text-eiu-deep sm:text-lg dark:text-white">{t('app.brand')}</span>
            <span className="block text-xs font-medium text-slate-700 dark:text-white/70">{t('app.platform')}</span>
          </span>
        </div>
        {/* lg:mb-20 lifts the copy about 40 px above the middle of the hero (the space-between gaps shrink evenly). */}
        <div className="max-w-md lg:mb-20">
          <h1 className="text-2xl leading-tight font-bold text-eiu-deep sm:text-3xl lg:text-4xl dark:text-white">{t('login.heroTitle')}</h1>
          <p className="mt-3 text-base text-slate-800 lg:mt-4 lg:text-lg dark:text-white/85">{t('login.heroBody')}</p>
          <p className="mt-3 text-sm font-semibold tracking-wide text-eiu-navy dark:text-white/90">{t('login.heroValues')}</p>
          <p className="mt-4 hidden max-w-sm text-sm text-slate-700 lg:block dark:text-white/75">{t('login.heroDetail')}</p>
        </div>
        <p className="hidden text-xs font-semibold tracking-[0.2em] text-eiu-navy/70 uppercase lg:block dark:text-white/50">{t('app.tagline')}</p>
        <RobotIllustration className="pointer-events-none absolute right-8 bottom-20 hidden h-44 w-auto opacity-90 lg:block" />
      </aside>

      {/* Controls, sign-in card and demo accounts form one group, 16 px apart. From lg two flexible spacers (1 : 1.6) set it a
          little above the vertical centre while there is room, and collapse when the group fills the column. */}
      <main className="flex flex-col items-center gap-4 px-4 py-8 sm:px-8 lg:py-10">
        <span aria-hidden className="hidden lg:block lg:grow" />
        <div className="flex w-full max-w-md items-center justify-between">
          <span className="lg:invisible" />
          <div className="flex items-center gap-2">
            <ThemeMenu tone="light" />
            <LanguageSwitch />
          </div>
        </div>

        <div className="w-full max-w-md rounded-2xl border border-ops-border bg-ops-card p-6 shadow-card sm:p-8">
          <h2 className="text-2xl font-bold text-slate-900">{t('login.title')}</h2>
          <p className="mt-1 text-sm text-slate-500">{t('login.subtitle')}</p>

          <form onSubmit={submit} className="mt-6 grid gap-4">
            <Field label={t('login.email')} htmlFor="email">
              <Input id="email" type="email" autoComplete="username" required value={email} onChange={(e) => setEmail(e.target.value)} />
            </Field>
            <Field label={t('login.password')} htmlFor="password">
              <div className="relative">
                <Input
                  id="password"
                  type={showPassword ? 'text' : 'password'}
                  autoComplete="current-password"
                  required
                  value={password}
                  onChange={(e) => setPassword(e.target.value)}
                  className="pr-12"
                />
                <IconButton
                  label={showPassword ? t('login.hidePassword') : t('login.showPassword')}
                  onClick={() => setShowPassword((v) => !v)}
                  className="absolute top-1 right-1"
                >
                  {showPassword ? <EyeOff className="size-5" /> : <Eye className="size-5" />}
                </IconButton>
              </div>
            </Field>
            {login.isError && <p role="alert" className="rounded-xl bg-red-50 px-4 py-3 text-sm font-medium text-red-700">{errorText(login.error)}</p>}
            <Button type="submit" size="lg" loading={login.isPending} icon={<LogIn className="size-5" />} className="mt-1 w-full">
              {login.isPending ? t('login.submitting') : t('login.submit')}
            </Button>
          </form>
          <p className="mt-5 text-center text-sm text-slate-500">{t('login.noAccount')}</p>
        </div>

        {config?.demoAccounts && config.demoAccounts.length > 0 && (
          <div className="w-full max-w-md rounded-2xl border border-dashed border-brand-300/60 bg-surface p-4 text-sm">
            <p className="mb-2 font-semibold text-slate-900">{t('login.demoAccounts')}</p>
            <ul className="space-y-2">
              {config.demoAccounts.map((a) => (
                <li key={a.email} className="flex items-center gap-3">
                  <span className="min-w-0 flex-1 truncate text-slate-600">
                    <span className="font-semibold">{t(a.role === 'admin' ? 'login.demoAdmin' : 'login.demoUser')}</span> · {a.email} / {a.password}
                  </span>
                  <Button size="sm" variant="soft" onClick={() => { setEmail(a.email); setPassword(a.password) }}>{t('login.use')}</Button>
                </li>
              ))}
            </ul>
          </div>
        )}
        <span aria-hidden className="hidden lg:block lg:grow-[1.6]" />
      </main>
    </div>
  )
}
