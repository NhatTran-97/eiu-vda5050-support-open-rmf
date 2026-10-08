import { Compass } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { LinkButton } from '../components/ui/Button'
import { Card } from '../components/ui/Card'
import { EmptyState } from '../components/ui/States'

export default function NotFoundPage() {
  const { t } = useTranslation()
  return (
    <Card className="mt-6">
      <EmptyState icon={<Compass />} title={t('pages.notFound.title')} body={t('pages.notFound.body')} action={<LinkButton to="/">{t('pages.notFound.action')}</LinkButton>} />
    </Card>
  )
}
