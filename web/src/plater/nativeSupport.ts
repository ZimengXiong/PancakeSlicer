import { objectValues } from '../profiles/view';
import type { PlateObject, State } from '../state/store';

const on = (v: unknown) => v === true || v === 1 || v === '1' || v === 'true';
const painted = (o: { paint?: PlateObject['paint'] }) => !!o.paint?.support && o.paint.support.size > 0;

export function nativeSupportConflicts(s: State): string | null {
  const lines: string[] = [];
  for (const o of s.objects) {
    const v = objectValues(s, o.id).values, style = String(v.support_style ?? '');
    const enabled = on(v.enable_support);
    if (style !== 'native' || !enabled) continue;
    if (on(v.support_on_build_plate_only))
      lines.push(`Native support doesn't support 'On build plate only' (object: ${o.name}). Turn it off or choose another support style.`);
    if ((o.volumes ?? []).some((v) => v.type === 'support_enforcer' || v.type === 'support_blocker'))
      lines.push(`Native support doesn't support support enforcers or blockers (object: ${o.name}). Remove them or choose another support style.`);
    if (painted(o) || (o.volumes ?? []).some(painted))
      lines.push(`Native support doesn't support painted support (object: ${o.name}). Remove it or choose another support style.`);
  }
  return lines.length ? lines.join('\n') : null;
}
