import type { Config, PresetType } from '../api/types';
import { openSlideOutEditor } from '../navigation';
import { createMachine } from '../flow/fsm';
import { documentGeneration } from '../state/history';
import { authoredChanges } from '../state/changes';
import { sameValue } from '../state/value-equality';
import { getState, presetEdits, resolvePreset, type State } from '../state/store';
import { slotFilaments } from './slots';
import { randomId } from '../random-id';

export interface ProfileSave {
  id: string; type: PresetType; slot: number; key: string; baseRevision: number; generation: number;
  edits: Config; from: { edits: Config; project: Config | null };
}
export function captureProfileSave(type: PresetType, slot = 0): ProfileSave {
  const s = getState(), key = (type === 'machine' ? s.printer : type === 'process' ? s.process : slotFilaments(s)[slot]) ?? '';
  const resolved = resolvePreset(s, type, key);
  if (!resolved.record || resolved.error) throw new Error(resolved.error ?? 'Select one of your custom profiles (or save as a new profile) to save these changes.');
  return { id: randomId(), type, slot, key: resolved.record.id, baseRevision: resolved.record.revision, generation: documentGeneration(),
    edits: structuredClone(authoredChanges(s, type, slot)),
    from: { edits: structuredClone(presetEdits(s, type, slot)), project: structuredClone((type === 'filament' ? s.projectPresets?.filaments[slot] : s.projectPresets?.[type]) ?? null) } };
}
export function reviewProjectProfileChanges(type: PresetType, slot = 0) {
  const save = captureProfileSave(type, slot);
  openSlideOutEditor(type, save.key);
}

export function acknowledgeProfileSave(s: State, save: ProfileSave, saved: Config): Partial<State> {
  const { type, slot } = save, key = type === 'machine' ? s.printer : type === 'process' ? s.process : slotFilaments(s)[slot];
  if (documentGeneration() !== save.generation || key !== save.key) return {};
  const edits = presetEdits(s, type, slot), project = (type === 'filament' ? s.projectPresets?.filaments[slot] : s.projectPresets?.[type]) ?? {};
  const nextEdits = { ...edits }, nextProject = { ...project };
  for (const k of Object.keys(saved)) {
    if (k in edits && sameValue(k, edits[k], save.from.edits[k])) delete nextEdits[k];
    if (k in project && sameValue(k, project[k], save.from.project?.[k])) delete nextProject[k];
  }
  return { ...(type === 'filament' ? { filamentEdits: { ...s.filamentEdits, [save.key]: nextEdits } } : { dirty: { ...s.dirty, [type]: nextEdits } }),
    ...(s.projectPresets ? { projectPresets: type === 'filament' ? { ...s.projectPresets, filaments: s.projectPresets.filaments.map((p, i) => i === slot ? nextProject : p) } : { ...s.projectPresets, [type]: nextProject } } : {}) };
}

export type ProfileSaveState = 'idle' | 'saving' | 'acked' | 'failed';
type ProfileSaveEvent = { type: 'START'; capture: string } | { type: 'ACKED'; capture: string; revision: number } | { type: 'FAILED'; capture: string; code: string };
interface ProfileSaveContext { capture: string | null; revision: number | null; code: string | null }
const start = { target: 'saving' as const, action: (_: ProfileSaveContext, e: { capture: string }) => ({ capture: e.capture, revision: null, code: null }) };
const mine = (c: ProfileSaveContext, e: { capture: string }) => e.capture === c.capture;
export const profileSaves = createMachine<ProfileSaveState, ProfileSaveEvent, ProfileSaveContext>({
  id: 'project-profile-save', initial: 'idle', strict: true, context: { capture: null, revision: null, code: null },
  states: {
    idle: { on: { START: start } },
    saving: { on: {
      ACKED: { target: 'acked', guard: mine, action: (_, e) => ({ revision: e.revision }) },
      FAILED: { target: 'failed', guard: mine, action: (_, e) => ({ code: e.code }) },
    } },
    acked: { on: { START: start } },
    failed: { on: { START: start } },
  },
});
