import { Lock } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { LinkButton } from '../components/ui/Button'
import { Card } from '../components/ui/Card'
import { EmptyState } from '../components/ui/States'

export default function ForbiddenPage() {
  const { t } = useTranslation()
  return (
    <Card className="mt-6">
      <EmptyState icon={<Lock />} title={t('pages.forbidden.title')} body={t('pages.forbidden.body')} action={<LinkButton to="/">{t('pages.forbidden.action')}</LinkButton>} />
    </Card>
  )
}
