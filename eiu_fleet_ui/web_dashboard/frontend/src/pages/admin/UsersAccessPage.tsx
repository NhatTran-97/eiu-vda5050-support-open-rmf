import { KeyRound, Search, ShieldCheck, UserPlus, Users } from 'lucide-react'
import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import { useSearchParams } from 'react-router'
import { useAccessCatalog, useAdminUsers, useConfig, useCreateUser, useMe, useSetUserPassword, useUpdateUser } from '../../api/queries'
import type { AccessCatalog, AdminUser, OperatorPermission, Role } from '../../api/types'
import { Button, IconButton } from '../../components/ui/Button'
import { Card, CardHeader } from '../../components/ui/Card'
import { Checkbox } from '../../components/ui/Checkbox'
import { DataTable, type Column } from '../../components/ui/DataTable'
import { Dialog } from '../../components/ui/Dialog'
import { Drawer } from '../../components/ui/Drawer'
import { Field, Input, Select } from '../../components/ui/Field'
import { EmptyState, ErrorState, Skeleton } from '../../components/ui/States'
import { Switch } from '../../components/ui/Switch'
import { PageHeader } from '../../layout/PageHeader'
import { cn } from '../../lib/cn'
import { useErrorText } from '../../lib/errors'
import { useFormat } from '../../lib/format'
import { useLocalize } from '../../lib/i18nText'
import { fold } from '../../lib/text'
import { toast } from '../../lib/toast'

const ROLES: Role[] = ['operator', 'admin']

interface AccessForm {
  fullName: string
  email: string
  department: string
  role: Role
  active: boolean
  password: string
  allowedServices: string[]
  allowedZones: string[]
  permissions: OperatorPermission[]
}

function toggle<T>(list: T[], value: T, on: boolean): T[] {
  return on ? [...new Set([...list, value])] : list.filter((v) => v !== value)
}

/** Edit Operator Access (or New Operator): role, services, zones and permissions an admin grants. */
function UserAccessDrawer({ user, catalog, self, onClose }: { user: AdminUser | null; catalog: AccessCatalog; self: boolean; onClose: () => void }) {
  const { t } = useTranslation()
  const localize = useLocalize()
  const errorText = useErrorText()
  const minimum = useConfig().data?.minPasswordLength ?? 8
  const create = useCreateUser()
  const update = useUpdateUser()
  const creating = user === null
  const [form, setForm] = useState<AccessForm>(() => ({
    fullName: user?.fullName ?? '', email: user?.email ?? '', department: user?.department ?? '', role: user?.role ?? 'operator',
    active: user?.active ?? true, password: '', allowedServices: user?.allowedServices ?? [], allowedZones: user?.allowedZones ?? [],
    permissions: (user?.permissions.filter((p) => catalog.permissionGroups.some((g) => g.permissions.includes(p as OperatorPermission))) as OperatorPermission[] | undefined)
      ?? [...catalog.defaults],
  }))
  const set = (patch: Partial<AccessForm>) => setForm((f) => ({ ...f, ...patch }))
  const admin = form.role === 'admin'
  const valid = form.fullName.trim() && (!creating || (form.email.includes('@') && form.password.length >= minimum))
  const pending = create.isPending || update.isPending

  const save = () => {
    const access = admin ? {} : { allowedServices: form.allowedServices, allowedZones: form.allowedZones, permissions: form.permissions }
    const done = { onSuccess: () => { toast.success(t(creating ? 'users.created' : 'users.updated')); onClose() }, onError: (e: unknown) => toast.error(errorText(e)) }
    if (creating) create.mutate({ email: form.email, fullName: form.fullName.trim(), role: form.role, password: form.password, department: form.department, active: form.active, ...access }, done)
    else update.mutate({ id: user.id, fullName: form.fullName.trim(), department: form.department, ...(self ? {} : { role: form.role, active: form.active }), ...access }, done)
  }

  return (
    <Drawer open onClose={onClose} width="form" title={creating ? t('users.newOperator') : t('users.editAccess')} subtitle={user?.email}
      footer={<><Button variant="ghost" onClick={onClose}>{t('common.cancel')}</Button>
        <Button disabled={!valid} loading={pending} onClick={save}>{t(creating ? 'users.createOperator' : 'admin.saveChanges')}</Button></>}>
      <div className="flex flex-col gap-6">
        <div className="grid gap-4">
          <Field label={t('users.col.user')} htmlFor="a-name"><Input id="a-name" value={form.fullName} onChange={(e) => set({ fullName: e.target.value })} /></Field>
          {creating && <Field label={t('users.col.email')} htmlFor="a-email"><Input id="a-email" type="email" autoComplete="off" value={form.email} onChange={(e) => set({ email: e.target.value })} /></Field>}
          <Field label={t('users.col.department')} htmlFor="a-dept"><Input id="a-dept" value={form.department} onChange={(e) => set({ department: e.target.value })} /></Field>
          <Field label={t('users.col.role')} htmlFor="a-role" hint={self ? t('admin.users.selfHint') : t(`users.roleHint.${form.role}`)}>
            <Select id="a-role" value={form.role} disabled={self} onChange={(e) => set({ role: e.target.value as Role })}>
              {ROLES.map((r) => <option key={r} value={r}>{t(`role.${r}`)}</option>)}
            </Select>
          </Field>
          {creating && (
            <Field label={t('admin.users.password')} htmlFor="a-pw" hint={t('admin.users.passwordHint', { min: minimum })}>
              <Input id="a-pw" type="password" autoComplete="new-password" value={form.password} onChange={(e) => set({ password: e.target.value })} />
            </Field>
          )}
          <Switch label={t('users.col.status')} hint={t('admin.users.activeHint')} checked={form.active} disabled={self} onChange={(active) => set({ active })} />
        </div>

        {admin ? (
          <p className="flex items-start gap-2 rounded-xl bg-brand-50 px-4 py-3 text-sm text-brand-800"><ShieldCheck className="mt-0.5 size-4 shrink-0" />{t('users.adminHasAll')}</p>
        ) : (
          <>
            <fieldset>
              <legend className="mb-2 text-sm font-bold text-slate-900">{t('users.services')}</legend>
              {catalog.services.map((s) => (
                <Checkbox key={s.id} label={localize(s.name)} hint={!s.enabled ? t('users.serviceDisabled') : undefined}
                  checked={form.allowedServices.includes(s.id)} onChange={(on) => set({ allowedServices: toggle(form.allowedServices, s.id, on) })} />
              ))}
              {form.allowedServices.length === 0 && <p className="mt-1 px-2 text-xs text-amber-700">{t('users.noServiceWarning')}</p>}
            </fieldset>
            <fieldset>
              <legend className="mb-2 text-sm font-bold text-slate-900">{t('users.zones')}</legend>
              {catalog.zones.map((z) => (
                <Checkbox key={z.id} label={localize(z.name)} checked={form.allowedZones.includes(z.id)} onChange={(on) => set({ allowedZones: toggle(form.allowedZones, z.id, on) })} />
              ))}
              <p className="mt-1 px-2 text-xs text-slate-500">{t('users.zonesHint')}</p>
            </fieldset>
            {catalog.permissionGroups.map((g) => (
              <fieldset key={g.id}>
                <legend className="mb-2 text-sm font-bold text-slate-900">{t(`users.permissionGroup.${g.id}`)}</legend>
                {g.permissions.map((p) => (
                  <Checkbox key={p} label={t(`permission.${p}`)} hint={t(`permissionHint.${p}`)} checked={form.permissions.includes(p)}
                    onChange={(on) => set({ permissions: toggle(form.permissions, p, on) })} />
                ))}
              </fieldset>
            ))}
          </>
        )}
      </div>
    </Drawer>
  )
}

function PasswordDialog({ user, onClose }: { user: AdminUser; onClose: () => void }) {
  const { t } = useTranslation()
  const set = useSetUserPassword()
  const errorText = useErrorText()
  const minimum = useConfig().data?.minPasswordLength ?? 8
  const [password, setPassword] = useState('')
  return (
    <Dialog open onClose={onClose} title={t('admin.users.resetTitle', { name: user.fullName })} footer={<>
      <Button variant="secondary" onClick={onClose}>{t('common.cancel')}</Button>
      <Button disabled={password.length < minimum} loading={set.isPending} onClick={() => set.mutate({ id: user.id, password }, {
        onSuccess: () => { toast.success(t('admin.users.passwordSet')); onClose() },
        onError: (e) => toast.error(errorText(e)),
      })}>{t('admin.users.resetPassword')}</Button>
    </>}>
      <Field label={t('admin.users.newPassword')} htmlFor="r-pw" hint={t('admin.users.resetHint', { min: minimum })}>
        <Input id="r-pw" type="password" autoComplete="new-password" value={password} onChange={(e) => setPassword(e.target.value)} />
      </Field>
    </Dialog>
  )
}

/** Users & Access: operator accounts and the services, zones and permissions each one has. */
export default function UsersAccessPage() {
  const { t } = useTranslation()
  const format = useFormat()
  const localize = useLocalize()
  const users = useAdminUsers()
  const catalog = useAccessCatalog().data
  const me = useMe().data
  const [params, setParams] = useSearchParams()
  const q = params.get('q') ?? ''
  const [editing, setEditing] = useState<{ user: AdminUser | null } | null>(null)
  const [password, setPassword] = useState<AdminUser | null>(null)
  const shown = (users.data ?? []).filter((u) => !q || fold(`${u.fullName} ${u.email} ${u.department}`).includes(fold(q)))
  const serviceName = (id: string) => localize(catalog?.services.find((s) => s.id === id)?.name ?? { vi: id, en: id })
  const zoneName = (id: string) => localize(catalog?.zones.find((z) => z.id === id)?.name ?? { vi: id, en: id })
  const chips = (items: string[], name: (id: string) => string) => (
    <span className="flex flex-wrap gap-1">{items.map((i) => <span key={i} className="rounded-md bg-slate-100 px-1.5 py-0.5 text-xs font-medium text-slate-700">{name(i)}</span>)}</span>
  )
  const columns: Column<AdminUser>[] = [
    { key: 'user', header: t('users.col.user'), cell: (u) => <span className="font-semibold whitespace-nowrap text-slate-900">{u.fullName}{u.id === me?.id && <span className="ml-1.5 text-xs font-medium text-slate-500">({t('admin.users.you')})</span>}</span> },
    { key: 'email', header: t('users.col.email'), cell: (u) => <span className="text-slate-600">{u.email}</span> },
    { key: 'role', header: t('users.col.role'), cell: (u) => <span className={cn('rounded-full px-2 py-0.5 text-xs font-semibold', u.role === 'admin' ? 'bg-brand-50 text-brand-700' : 'bg-slate-100 text-slate-700')}>{t(`role.${u.role}`)}</span> },
    { key: 'department', header: t('users.col.department'), cell: (u) => u.department || <span className="text-slate-400">—</span> },
    { key: 'services', header: t('users.col.services'), cell: (u) => (u.role === 'admin' ? <span className="text-xs text-slate-500">{t('users.all')}</span> : u.allowedServices.length ? chips(u.allowedServices, serviceName) : <span className="text-xs text-amber-700">{t('users.none')}</span>) },
    { key: 'zones', header: t('users.col.zones'), cell: (u) => (u.role === 'admin' || u.allowedZones.length === 0 ? <span className="text-xs text-slate-500">{t('users.allZones')}</span> : chips(u.allowedZones, zoneName)) },
    { key: 'status', header: t('users.col.status'), cell: (u) => (u.active ? <span className="text-emerald-700">{t('admin.users.enabled')}</span> : <span className="text-slate-500">{t('admin.users.disabled')}</span>) },
    { key: 'active', header: t('users.col.lastActive'), cell: (u) => <span className="whitespace-nowrap text-slate-600">{u.lastActiveAt ? format.dateTime(u.lastActiveAt) : '—'}</span> },
    { key: 'actions', header: <span className="sr-only">{t('fleet.col.actions')}</span>, cell: (u) => (
      <span className="flex justify-end gap-1">
        <Button size="sm" variant="secondary" onClick={() => setEditing({ user: u })}>{t('users.editAccess')}</Button>
        <IconButton label={t('admin.users.resetPassword')} onClick={() => setPassword(u)}><KeyRound className="size-4" /></IconButton>
      </span>
    ) },
  ]
  return (
    <>
      <PageHeader title={t('users.title')} subtitle={t('users.subtitle')}
        actions={<Button icon={<UserPlus className="size-4" />} disabled={!catalog} onClick={() => setEditing({ user: null })}>{t('users.newOperator')}</Button>} />
      <Card className="p-4 sm:p-5">
        <CardHeader icon={<Users />} title={t('admin.users.accounts', { count: users.data?.length ?? 0 })} className="mb-4" />
        <div className="relative mb-3 max-w-md">
          <Search className="pointer-events-none absolute top-1/2 left-3.5 size-4 -translate-y-1/2 text-slate-400" />
          <Input value={q} onChange={(e) => setParams(e.target.value ? { q: e.target.value } : {}, { replace: true })}
            placeholder={t('admin.users.search')} aria-label={t('admin.users.search')} className="h-11 pl-10" />
        </div>
        {users.isPending && <Skeleton className="h-48" />}
        {users.isError && <ErrorState onRetry={() => void users.refetch()} />}
        {users.data && <DataTable columns={columns} rows={shown} rowKey={(u) => u.id} minWidth={1180} empty={<EmptyState icon={<Users />} title={t('admin.users.empty')} />} />}
      </Card>
      {editing && catalog && <UserAccessDrawer key={editing.user?.id ?? 'new'} user={editing.user} catalog={catalog} self={editing.user?.id === me?.id} onClose={() => setEditing(null)} />}
      {password && <PasswordDialog user={password} onClose={() => setPassword(null)} />}
    </>
  )
}
