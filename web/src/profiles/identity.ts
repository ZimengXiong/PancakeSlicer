import type { PresetType } from '../api/types';
import type { FilamentRecord, PrinterRecord, ProcessRecord } from './records';
import type { State } from '../state/store';
import { slotFilaments } from './slots';

export type ProfileKey = string;
export interface ProfileReference {
  kind: 'active' | 'default' | 'project' | 'inheritance';
  label: string;
  owner?: ProfileKey;
}
export interface ProfileReferenceSource {
  complete: boolean;
  references: readonly { type: PresetType; key: ProfileKey; reference: ProfileReference }[];
}
export interface ProfileReferenceInventory { complete: boolean; references: readonly ProfileReference[] }

export type LibraryProfile = (PrinterRecord | FilamentRecord | ProcessRecord) & { type: PresetType };
export const profileKey = (profile: { id: string }): ProfileKey => profile.id;
export const matchesProfile = (profile: { id: string; name: string; aliases?: string[] }, key: ProfileKey) =>
  profile.id === key || profile.name === key || !!profile.aliases?.includes(key);

const indexes = new WeakMap<object, Map<PresetType, { ids: Map<string, LibraryProfile | null>; names: Map<string, LibraryProfile | null>; aliases: Map<string, LibraryProfile | null> }>>();
const put = (map: Map<string, LibraryProfile | null>, key: string, p: LibraryProfile) => map.set(key, map.has(key) ? null : p);
export function lookupProfile(s: Pick<State, 'records'>, type: PresetType, key: ProfileKey): { profile?: LibraryProfile; ambiguous: boolean } {
  let byType = indexes.get(s.records);
  if (!byType) indexes.set(s.records, (byType = new Map()));
  let index = byType.get(type);
  if (!index) {
    index = { ids: new Map(), names: new Map(), aliases: new Map() };
    const list = type === 'machine' ? s.records.printers : type === 'process' ? s.records.processes : s.records.filaments;
    for (const record of list) {
      const p = { ...record, type } as LibraryProfile;
      put(index.ids, p.id, p); put(index.names, p.name, p);
      for (const alias of new Set((p as FilamentRecord).aliases ?? [])) put(index.aliases, alias, p);
    }
    byType.set(type, index);
  }
  for (const map of [index.ids, index.names, index.aliases]) if (map.has(key)) {
    const profile = map.get(key);
    return { profile: profile ?? undefined, ambiguous: profile === null };
  }
  return { ambiguous: false };
}
export const findProfile = (s: Pick<State, 'records'>, type: PresetType, key: ProfileKey): LibraryProfile | undefined => lookupProfile(s, type, key).profile;

export function profileReferenceInventory(s: State, type: PresetType, key: ProfileKey, external?: ProfileReferenceSource): ProfileReferenceInventory {
  const p = findProfile(s, type, key);
  if (!p) return { complete: false, references: [] };
  const references: ProfileReference[] = [];
  const selected = type === 'machine' ? [s.printer] : type === 'process' ? [s.process] : slotFilaments(s);
  selected.forEach((selectedKey, slot) => {
    if (!matchesProfile(p, selectedKey)) return;
    references.push({ kind: 'active', label: type === 'filament' ? `Filament ${slot + 1}` : type === 'machine' ? 'Printer' : 'Process' });
    if (s.project || s.projectName) references.push({ kind: 'project', label: s.projectName ?? s.project?.name ?? 'Current project' });
  });
  for (const item of external?.references ?? []) if (item.type === type && matchesProfile(p, item.key)) references.push(item.reference);
  return { complete: external?.complete === true, references };
}
