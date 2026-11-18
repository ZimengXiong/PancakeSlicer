import { createMachine, type StateNode } from './fsm';
import { movesText, remoteProfileWrites, type SettingMove } from './profile-save';
import { getState, toast } from '../state/store';
import type { PresetType } from '../api/types';

function profileName(key: string): string {
  const [type, id] = JSON.parse(key) as [PresetType, string], r = getState().records;
  const list: { id: string; name: string }[] = type === 'machine' ? r.printers : type === 'process' ? r.processes : r.filaments;
  return list.find((p) => p.id === id)?.name ?? 'A profile';
}

export type SyncState = 'live' | 'reconnecting';
export type ProfileMoves = [string, SettingMove[]][];
export type SyncEvent = { type: 'STREAM_LOST' } | { type: 'STREAM_BACK' }
  | { type: 'SETUP_CHANGED_ELSEWHERE'; moves: ProfileMoves } | { type: 'RECORD_CHANGED_ELSEWHERE'; moves: ProfileMoves }
  | { type: 'IN_USE_REMOVED_ELSEWHERE'; lines: string[] } | { type: 'CONFLICT_ELSEWHERE'; conflicts: ProfileMoves }
  | { type: 'EDITOR_SHOWN'; key: string; visible: () => boolean } | { type: 'EDITOR_HIDDEN'; key: string; visible: () => boolean }
  | { type: 'STREAM_OPENER'; open: () => void };
interface SyncContext {  shown: Record<string, (() => boolean)[]>;
   attempt: number; open: (() => void) | null }
type Node = StateNode<SyncState, SyncEvent, SyncContext>;

function say(c: SyncContext, moves: ProfileMoves) {
  const onScreen = (key: string) => !!c.shown[key]?.some((visible) => visible());
  const inEditor = moves.filter(([key, m]) => m.length && onScreen(key)), rest = moves.filter(([key, m]) => m.length && !onScreen(key));
  const told = [...rest, ...remoteProfileWrites([], inEditor)];
  if (told.length) toast('heads_up_updated_another', { value: told.map(([key, m]) => `${profileName(key)}: ${movesText(m)}`).join('; ') });
}
const elsewhere: Node['on'] = {
  SETUP_CHANGED_ELSEWHERE: { action: (c, e) => { say(c, e.moves); } },
  RECORD_CHANGED_ELSEWHERE: { action: (c, e) => { say(c, e.moves); } },
  IN_USE_REMOVED_ELSEWHERE: { action: (_, e) => { if (e.lines.length) toast('in_use_removed_elsewhere', { lines: [...new Set(e.lines)].join('\n') }); } },
  CONFLICT_ELSEWHERE: { action: (_, e) => { remoteProfileWrites(e.conflicts); } },
  EDITOR_SHOWN: { action: (c, e) => ({ shown: { ...c.shown, [e.key]: [...c.shown[e.key] ?? [], e.visible] } }) },
  EDITOR_HIDDEN: { action: (c, e) => { const shown = { ...c.shown }, left = (shown[e.key] ?? []).filter((v) => v !== e.visible); if (left.length) shown[e.key] = left; else delete shown[e.key]; return { shown }; } },
  STREAM_OPENER: { action: (_, e) => ({ open: e.open }) },
};
const BACKOFF = [1000, 1000, 1000, 1000, 1000, 2000, 4000];

export const librarySync = createMachine<SyncState, SyncEvent, SyncContext>({
  id: 'library-sync', initial: 'live', strict: true, context: { shown: {}, attempt: 0, open: null },
  states: {
    live: { on: { ...elsewhere, STREAM_LOST: { target: 'reconnecting', action: () => ({ attempt: 0 }) }, STREAM_BACK: {} } },
    reconnecting: { note: 'the live stream is down; the library shows "Reconnecting…"; the leader reopens it on a backoff',
      after: { ms: (c) => BACKOFF[Math.min(c.attempt, BACKOFF.length - 1)], target: 'reconnecting', action: (c) => { c.open?.(); return { attempt: c.attempt + 1 }; } },
      on: { ...elsewhere, STREAM_BACK: 'live', STREAM_LOST: {} } },
  },
});
