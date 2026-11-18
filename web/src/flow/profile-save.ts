import { NAME_MAX, nameLength } from '../profiles/records';
import { createMachine, type StateNode } from './fsm';

export type ProfileSaveState = 'idle' | 'saving' | 'failed' | 'naming' | 'renaming' | 'conflict';
export interface SettingMove { label: string; from: string; to: string }
interface ProfileSaveContext { request: number; error: string | null; name: string; nameError: string | null;  updated: SettingMove[] | null;
   conflict: SettingMove[] | null }
type ProfileSaveEvent = { type: 'SAVE' } | { type: 'REJECT'; error: string } | { type: 'SAVED'; request: number }
  | { type: 'FAIL'; request: number; error: string } | { type: 'RENAME'; name: string } | { type: 'TYPE'; value: string }
  | { type: 'COMMIT'; value: string; current: string; taken: boolean } | { type: 'RENAMED'; request: number }
  | { type: 'RENAME_FAIL'; request: number; error: string } | { type: 'CANCEL' } | { type: 'DISMISS' }
  | { type: 'REMOTE'; moves?: SettingMove[] } | { type: 'UPDATED'; moves: SettingMove[] } | { type: 'SEEN' } | { type: 'KEEP' } | { type: 'DISCARD' };
type Node = StateNode<ProfileSaveState, ProfileSaveEvent, ProfileSaveContext>;

const mine = (c: ProfileSaveContext, e: { request: number }) => e.request === c.request;
const late = {} as const;
const save = { target: 'saving', action: (c: ProfileSaveContext) => ({ request: c.request + 1, error: null, updated: null }) } as const;
const rename = { target: 'naming', action: (_: ProfileSaveContext, e: Extract<ProfileSaveEvent, { type: 'RENAME' }>) => ({ name: e.name, nameError: null }) } as const;
const reject = { target: 'failed', action: (_: ProfileSaveContext, e: Extract<ProfileSaveEvent, { type: 'REJECT' }>) => ({ error: e.error }) } as const;
const results = { SAVED: late, FAIL: late, RENAMED: late, RENAME_FAIL: late, DISMISS: late, KEEP: late, DISCARD: late, UPDATED: late,
  SEEN: { action: () => ({ updated: null }) } } as const;
const remote = { REMOTE: { target: 'conflict', action: (_: ProfileSaveContext, e: { moves?: SettingMove[] }) => ({ conflict: e.moves ?? null }) } } as const;
const ready: Node = { on: { ...results, ...remote, UPDATED: { action: (c, e) => ({ updated: mergeMoves(c.updated, e.moves) }) }, SAVE: save, REJECT: reject, RENAME: rename, DISMISS: { target: 'idle', action: () => ({ error: null }) } } };

export const createProfileSave = () => createMachine<ProfileSaveState, ProfileSaveEvent, ProfileSaveContext>({
  id: 'profile-save', initial: 'idle', strict: true, context: { request: 0, error: null, name: '', nameError: null, updated: null, conflict: null },
  states: {
    idle: ready,
    failed: ready,
    saving: { on: { ...results, REMOTE: late,
      SAVED: [{ target: 'idle', guard: mine }, late],
      FAIL: [{ target: 'failed', guard: mine, action: (_, e) => ({ error: e.error }) }, late],
    } },
    naming: { on: { ...results, REMOTE: late,
      TYPE: { action: (_, e) => ({ name: e.value, nameError: null }) },
      COMMIT: [
        { target: 'idle', guard: (_, e) => e.value.trim() === e.current },
        { guard: (_, e) => !e.value.trim(), action: () => ({ nameError: 'Enter a name' }) },
        { guard: (_, e) => nameLength(e.value.trim()) > NAME_MAX, action: () => ({ nameError: `Keep the name to ${NAME_MAX} characters` }) },
        { guard: (_, e) => e.taken, action: (_, e) => ({ nameError: `“${e.value.trim()}” is already taken` }) },
        { target: 'renaming', action: (c) => ({ request: c.request + 1 }) },
      ],
      CANCEL: { target: 'idle', action: () => ({ nameError: null }) },
    } },
    renaming: { on: { ...results, REMOTE: late, COMMIT: late,
      RENAMED: [{ target: 'idle', guard: mine, action: () => ({ nameError: null }) }, late],
      RENAME_FAIL: [{ target: 'naming', guard: mine, action: (_, e) => ({ nameError: e.error }) }, late],
    } },
    conflict: { on: { ...results, REMOTE: late, REJECT: reject,
      KEEP: { target: 'idle', action: () => ({ error: null, conflict: null }) },
      DISCARD: { target: 'idle', action: () => ({ error: null, conflict: null }) },
    } },
  },
});

function mergeMoves(seen: SettingMove[] | null, next: SettingMove[]): SettingMove[] | null {
  const out = new Map((seen ?? []).map((m) => [m.label, m]));
  for (const m of next) out.set(m.label, m);
  const moves = [...out.values()].filter((m) => m.from !== m.to);
  return moves.length ? moves : null;
}
export const movesText = (moves: SettingMove[]) => moves.length > 3 ? `${moves.length} settings changed` : moves.map((m) => `${m.label} ${m.from} → ${m.to}`).join(', ');

const shared = new Map<string, ReturnType<typeof createProfileSave>>();
export function profileSaveFor(draftKey: string) {
  let m = shared.get(draftKey);
  if (!m) shared.set(draftKey, m = createProfileSave());
  return m;
}
export function remoteProfileWrites(conflicts: readonly [string, SettingMove[]][], updated: readonly [string, SettingMove[]][] = []): [string, SettingMove[]][] {
  for (const [key, moves] of conflicts) { const m = profileSaveFor(key); if (m.can({ type: 'REMOTE', moves })) m.send({ type: 'REMOTE', moves }); }
  const missed: [string, SettingMove[]][] = [];
  for (const [key, moves] of updated) { const m = shared.get(key); if (moves.length && m?.can({ type: 'UPDATED', moves })) m.send({ type: 'UPDATED', moves }); else if (moves.length) missed.push([key, moves]); }
  return missed;
}
