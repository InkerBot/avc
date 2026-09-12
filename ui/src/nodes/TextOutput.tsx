import { useLayoutEffect, useRef } from 'react'
import { useTranslation } from 'react-i18next'
import type { TextConversation } from '../store'

export function TextOutput({ conversation }: { conversation?: TextConversation }) {
  const output = useRef<HTMLPreElement>(null)
  const follow = useRef(true)
  const { t } = useTranslation()
  const text = [
    ...(conversation?.history.map((message) => message.text) ?? []),
    conversation?.draft?.text ?? '',
  ].filter(Boolean).join('\n')

  useLayoutEffect(() => {
    const element = output.current
    if (element && follow.current) element.scrollTop = element.scrollHeight
  }, [text])

  return (
    <pre
      ref={output}
      className={`node__text-output nodrag nowheel${text ? '' : ' node__text-output--empty'}`}
      onScroll={(event) => {
        const element = event.currentTarget
        follow.current = element.scrollHeight - element.scrollTop - element.clientHeight < 4
      }}
      aria-live="polite"
      aria-atomic="true"
    >
      {text || t('node.waitingForText')}
    </pre>
  )
}
