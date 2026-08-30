export const categoryColor: Record<string, string> = {
  io: '#4C8DFF',
  basic: '#6E9A82',
  eq: '#C89A2A',
  dyn: '#C4553F',
  voice: '#A356C4',
  fx: '#3E9E9E',
  debug: '#8A8F98',
}

export function colorOf(category: string): string {
  return categoryColor[category] ?? '#7A7480'
}

export const portTypeColor: Record<string, string> = {
  audio: '#6E9A82',
  text: '#C89A2A',
}

export function portColorOf(type: string): string {
  const known = portTypeColor[type]
  if (known) return known

  let hash = 0
  for (let i = 0; i < type.length; i += 1) hash = (hash * 31 + type.charCodeAt(i)) | 0
  return `hsl(${Math.abs(hash) % 360}, 42%, 58%)`
}

export function levelColor(db: number): string {
  if (db > -1) return '#E0554A'
  if (db > -9) return '#D9A63C'
  return '#4FB07A'
}

export function levelFraction(db: number): number {
  return Math.max(0, Math.min(1, (db + 60) / 60))
}
