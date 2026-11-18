import type { PresetType } from '../api/types';
import { type State } from '../state/store';
import { selectCompatibility } from './compatibility';
const recordsOf = (s: State, type: PresetType) => type === 'machine' ? s.records.printers : type === 'process' ? s.records.processes : s.records.filaments;

export interface ProfileRow { key: string; name: string; mine: true }

const profileRows = new WeakMap<State, Map<PresetType, ProfileRow[]>>();
export function listProfiles(s: State, type: PresetType, filters: { browseAll?: boolean } = {}): ProfileRow[] {
  let cache = profileRows.get(s);
  if (!cache) profileRows.set(s, (cache = new Map()));
  if (!filters.browseAll && cache.has(type)) return cache.get(type)!;
  const rows: ProfileRow[] = recordsOf(s, type).filter((r) => !r.archived && (filters.browseAll || selectCompatibility(s, type, r.id).ok))
    .map((r) => ({ key: r.id, name: r.name, mine: true as const }));
  if (!filters.browseAll) cache.set(type, rows);
  return rows;
}
export function uniqueName(s: State, type: PresetType, desired: string): string {
  let name = desired, n = 2;
  while (recordsOf(s, type).some((r) => !r.archived && r.name === name)) name = `${desired} (${n++})`;
  return name;
}
