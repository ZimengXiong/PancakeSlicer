import type { PresetType } from '../api/types';
import { resolvePreset, type State } from '../state/store';
import { recordOf } from './view';
export interface Compatibility { ok: boolean; reason: string; unknown?: boolean }
const allowed = (): Compatibility => ({ok:true,reason:''});
const denied = (reason: string, unknown = false): Compatibility => ({ok:false,reason,...(unknown ? {unknown:true} : {})});
export function selectCompatibility(s: State, type: PresetType, key: string): Compatibility {
  const record = recordOf(s, type, key);
  if (!record || record.archived) return denied('This profile was removed from your library.', true);
  if (type === 'machine') return allowed();
  const r = resolvePreset(s, type, key);
  return r.state === 'ready' && r.error ? denied(r.error, true) : allowed();
}
