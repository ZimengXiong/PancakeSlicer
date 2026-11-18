import { recordOf, valueBelow } from './view';
import type { Config, PresetType } from '../api/types';
import { getState, resolvePreset, setOption, setState, type State } from '../state/store';
import { authoredChanges, selectChanges } from '../state/changes';
import { sameValue } from '../state/value-equality';
import { undoable } from '../state/history';
import { captureProfileSave } from './project-draft';
import { revert, revertAll } from './stack';
import type { ProfileDraft } from './ProfileEditor';
import { changedKeys } from './model';
import { slotFilaments } from './slots';

export function inUseSlot(s: State, type: PresetType, key: string): number {
  if (type === 'machine') return key === s.printer ? 0 : -1;
  if (type === 'process') return key === s.process ? 0 : -1;
  return slotFilaments(s).indexOf(key);
}

const projectChanges = (s: State, type: PresetType, slot: number) =>
  selectChanges(s, type, slot).filter((c) => c.scope === 'project' && c.owner.plate === undefined);

export const projectDeltaKey = (s: State, type: PresetType, key: string) => {
  const slot = inUseSlot(s, type, key);
  return slot < 0 ? '' : JSON.stringify([slot, authoredChanges(s, type, slot)]);
};

function savedValues(s: State, type: PresetType, key: string): Config {
  const resolved = resolvePreset(s, type, key), out: Config = { ...resolved.config };
  for (const [k, l] of Object.entries(resolved.layers)) {
    if (l.project === undefined) continue;
    const below = valueBelow(l, 'project');
    if (below === undefined) delete out[k]; else out[k] = below;
  }
  return out;
}

export function sharedDraft(s: State, type: PresetType, key: string, deltaKey: string, stored: ProfileDraft | undefined): ProfileDraft | undefined {
  if (!deltaKey) return stored;
  const [slot, deltas] = JSON.parse(deltaKey) as [number, Config];
  if (!Object.keys(deltas).length && !stored) return undefined;
  const resolved = resolvePreset(s, type, key), source = recordOf(s, type, key);
  if (!source || resolved.error) return stored;
  const saved = savedValues(s, type, key);
  return { ...stored, values: { ...saved, ...deltas }, baseline: saved, origin: stored?.origin ?? source, printer: s.printer,
    staged: Object.keys(deltas).length ? captureProfileSave(type, slot) : undefined };
}

const editorOnly = (d: ProfileDraft) => d.scales !== undefined || d.speedModel !== undefined || d.speedOverrides !== undefined || d.printOverrides !== undefined || !!d.cleared?.length || !!d.locked?.length;

export function writeSharedDraft(draftKey: string, next: ProfileDraft | null) {
  const [type, key] = JSON.parse(draftKey) as [PresetType, string];
  const s = getState(), slot = inUseSlot(s, type, key);
  const store = (d: ProfileDraft | undefined) => setState((st) => {
    const drafts = { ...st.libraryDrafts };
    if (d) drafts[draftKey] = d; else delete drafts[draftKey];
    return { libraryDrafts: drafts };
  });
  if (slot < 0) { store(next ?? undefined); return; }
  if (!next) { revertAll(type, slot); store(undefined); return; }
  const changes = projectChanges(s, type, slot), current = authoredChanges(s, type, slot);
  const baseline = savedValues(s, type, key), keys = new Set([...Object.keys(next.values), ...Object.keys(current)]);
  const edits = [...keys].filter((k) => !sameValue(k, next.values[k], current[k] ?? baseline[k]));
  if (edits.length) undoable('Edit profile', () => {
    for (const k of edits) {
      const change = changes.find((c) => c.key === k);
      if (sameValue(k, next.values[k], baseline[k])) { if (change) revert(change); }
      else if (next.values[k] !== undefined) setOption(type, k, next.values[k]!, slot);
    }
  });
  store(editorOnly(next) ? { ...next, values: baseline, baseline } : undefined);
}

export const updateSharedDraft = (key: string, draft: ProfileDraft | null) => writeSharedDraft(key, draft && (Object.keys(changedKeys(draft.values, draft.baseline)).length
  || draft.cleared?.length || draft.locked?.length || draft.scales || draft.speedModel !== undefined || draft.speedOverrides || draft.printOverrides) ? draft : null);

export function removedInUseDrafts(before: State, library: State['records'], mine: (type: PresetType, key: string) => boolean): Partial<State> {
  const used: [PresetType, string][] = [['machine', before.printer], ['process', before.process], ...slotFilaments(before).map((k) => ['filament', k] as [PresetType, string])];
  let drafts = before.libraryDrafts, filamentEdits = before.filamentEdits, dirty = before.dirty;
  for (const [type, key] of used) {
    const k = JSON.stringify([type, key]), delta = projectDeltaKey(before, type, key);
    if (!key || !delta || mine(type, key) || !Object.keys(JSON.parse(delta)[1]).length) continue;
    const list = (type === 'machine' ? library.printers : type === 'process' ? library.processes : library.filaments) as { id: string; archived?: unknown }[];
    const now = list.find((r) => r.id === key);
    if (now && !now.archived) continue;
    const draft = sharedDraft(before, type, key, delta, before.libraryDrafts[k]);
    if (!draft) continue;
    drafts = { ...drafts, [k]: { ...draft, staged: undefined } };
    if (type === 'filament') { filamentEdits = { ...filamentEdits }; delete filamentEdits[key]; }
    else dirty = { ...dirty, [type]: {} };
  }
  return drafts === before.libraryDrafts ? {} : { libraryDrafts: drafts, filamentEdits, dirty };
}

export function rebasedProfileDraft(current: ProfileDraft, origin: ProfileDraft['origin'], baseline: Config): ProfileDraft {
  const values = { ...baseline, ...changedKeys(current.values, current.baseline) };
  for (const key of current.cleared ?? []) if (current.values[key] === undefined) delete values[key]; else values[key] = current.values[key]!;
  return { ...current, values, baseline, origin };
}

export function draftChanges(s: State, type: PresetType, key: string): Config {
  const k = JSON.stringify([type, key]), delta = projectDeltaKey(s, type, key), stored = s.libraryDrafts[k];
  return { ...(stored ? changedKeys(stored.values, stored.baseline) : {}), ...(delta ? JSON.parse(delta)[1] as Config : {}) };
}

export function sharedChanges<T>(before: State, after: State, move: (key: string, from: Config[string] | undefined, to: Config[string] | undefined) => T): [string, T[]][] {
  const used: [PresetType, string][] = [['machine', after.printer], ['process', after.process], ...slotFilaments(after).map((k) => ['filament', k] as [PresetType, string])];
  const out: [string, T[]][] = [];
  for (const [type, key] of new Map(used.map((u) => [JSON.stringify(u), u])).values()) {
    if (!key) continue;
    const deltas = (s: State): Config => (type === 'filament' ? s.filamentEdits[key] : s.dirty[type as 'machine' | 'process']) ?? {};
    const was = deltas(before), now = deltas(after);
    const savedBefore = savedValues(before, type, key), savedAfter = savedValues(after, type, key);
    const keys = [...new Set([...Object.keys(was), ...Object.keys(now)])].filter((k) => !sameValue(k, was[k] ?? savedBefore[k], now[k] ?? savedAfter[k]));
    if (!keys.length) continue;
    out.push([JSON.stringify([type, key]), keys.map((k) => move(k, was[k] ?? savedBefore[k], now[k] ?? savedAfter[k]))]);
  }
  return out;
}
