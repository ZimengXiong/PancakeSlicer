import type { PresetType } from '../api/types';
import { resolvePreset, type State } from '../state/store';
import { printerOverrides, type SettingOrigin } from './context';
import { SPEED_KEYS } from './speed-rules';
import type { ProcessRecord } from './records';

export type { SettingOrigin };
export type SettingSource = SettingOrigin;
const sourceCache = new WeakMap<State, Map<string, Record<string, SettingOrigin>>>();
export function settingSources(s: State, type: PresetType, key: string): Record<string, SettingOrigin> {
  let byProfile = sourceCache.get(s);
  if (!byProfile) sourceCache.set(s, (byProfile = new Map()));
  const id = `${type}:${key}`, hit = byProfile.get(id);
  if (hit) return hit;
  const resolved = resolvePreset(s, type, key), out: Record<string, SettingOrigin> = {};
  for (const [k, l] of Object.entries(resolved.layers)) if (l.auto !== undefined) out[k] = 'auto';
  if (type === 'process') {
    const record = resolved.record as ProcessRecord | undefined;
    for (const row of record?.scales ?? []) out[row.key] = 'auto';
    const dynamic = !!record?.speedModel;
    if (key === s.process) for (const k of Object.keys(printerOverrides(s)))
      if (!(k in out) && resolved.layers[k]?.project === undefined && !(dynamic && SPEED_KEYS.has(k))) out[k] = 'printer';
  }
  byProfile.set(id, out);
  return out;
}
