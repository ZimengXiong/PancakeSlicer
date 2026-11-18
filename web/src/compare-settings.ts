import { OPTIONS, isHidden } from './options';
import { effectiveConfig, type State } from './state/store';
import { slotFilaments } from './profiles/slots';

type Config = Record<string, unknown>;
export interface SettingChange { label: string; from: string; to: string }

export const resolvedOf = (signature: string | undefined): Config[] => {
  try { return (JSON.parse(signature ?? '{}') as { resolved?: Config[] }).resolved ?? []; } catch { return []; }
};
export const editedOf = (signature: string | undefined): Set<string> => {
  try { return new Set(Object.keys((JSON.parse(signature ?? '{}') as { overrides?: Config }).overrides ?? {})); } catch { return new Set(); }
};
const SHOWN_AS: Record<string, string> = { nonplanar_mode: 'Slicing method' };
const resolvedNow = (s: State): Config[] => [effectiveConfig(s, 'machine'), effectiveConfig(s, 'process'), ...slotFilaments(s).map((_, i) => effectiveConfig(s, 'filament', i))];

function shown(key: string, value: unknown): string {
  const def = OPTIONS[key];
  const one = (v: unknown): string => {
    const text = String(v ?? '');
    if (def?.type === 'bool' || def?.type === 'bools') return text === '1' || text === 'true' ? 'On' : 'Off';
    const at = def?.enum_values?.indexOf(text) ?? -1;
    if (at >= 0 && def?.enum_labels?.[at]) return def.enum_labels[at];
    const number = text !== '' && Number.isFinite(Number(text)) ? String(Math.round(Number(text) * 100) / 100) : null;
    return number != null ? def?.sidetext ? `${number} ${def.sidetext}` : number : text || '–';
  };
  return Array.isArray(value) ? [...new Set(value.map(one))].join(', ') : one(value);
}

export function settingChanges(before: Config[], after: Config[], first: Set<string> = new Set()): SettingChange[] {
  const lead: SettingChange[] = [];
  const out: SettingChange[] = [], slots = Math.max(before.length, after.length);
  for (let i = 0; i < slots; i++) {
    const a = before[i] ?? {}, b = after[i] ?? {};
    for (const key of new Set([...Object.keys(a), ...Object.keys(b)])) {
      const def = OPTIONS[key];
      if (!def?.label || isHidden(key) || JSON.stringify(a[key]) === JSON.stringify(b[key])) continue;
      const name = SHOWN_AS[key] ?? (def.full_label || def.label);
      (first.has(key) || key === 'layer_height' ? lead : out).push({ label: i >= 2 && slots > 3 ? `Filament ${i - 1}: ${name}` : name, from: shown(key, a[key]), to: shown(key, b[key]) });
    }
  }
  return [...lead, ...out];
}

export function settingsChangedSinceSlice(s: State): boolean {
  const job = s.job;
  return job.state === 'done' && !!job.stale && !!job.id && settingChanges(resolvedOf(job.plateSignature), resolvedNow(s)).length > 0;
}
