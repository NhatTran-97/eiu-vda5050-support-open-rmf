import { ChevronRight } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { Link } from 'react-router'
import { Card } from '../components/ui/Card'
import { PageHeader } from '../layout/PageHeader'
import { useNav } from '../layout/Sidebar'

/** Phone menu: every section the user may open. */
export default function MenuPage() {
  const { t } = useTranslation()
  const groups = useNav()
  return (
    <>
      <PageHeader title={t('nav.menu')} />
      <div className="flex flex-col gap-4">
        {[...groups, { title: null, items: [{ to: '/account', label: 'nav.account', icon: ChevronRight }] }].map((g, i) => (
          <Card key={i} className="divide-y divide-slate-100">
            {g.title && <p className="px-4 py-2.5 text-xs font-semibold tracking-wider text-slate-500 uppercase">{t(g.title)}</p>}
            {g.items.map((item) => {
              const Icon = item.icon
              return (
                <Link key={item.to} to={item.to} className="flex items-center gap-3 px-4 py-3.5 text-slate-900 hover:bg-slate-50">
                  <Icon className="size-5 text-slate-500" /><span className="flex-1 font-medium">{t(item.label)}</span><ChevronRight className="size-4 text-slate-400" />
                </Link>
              )
            })}
          </Card>
        ))}
      </div>
    </>
  )
}
