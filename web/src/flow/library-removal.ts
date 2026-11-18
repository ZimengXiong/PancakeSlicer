import type { PresetType } from '../api/types';
import { createMachine } from './fsm';
import type { RecordRemoval } from '../profiles/transactions';
import { removeRecords, restoreRecords } from '../profiles/transactions';

type RemovalState = 'idle' | 'removing' | 'removed' | 'restoring';
type RemovalEvent = { type: 'REMOVE'; kind: PresetType; keys: string[] } | { type: 'REMOVED'; request: number; undo: RecordRemoval }
  | { type: 'UNDO' } | { type: 'RESTORED'; request: number } | { type: 'FAIL'; request: number; error: string };
interface RemovalContext { request: number; undo: RecordRemoval | null; error: string | null }

const mine = (c: RemovalContext, e: { request: number }) => e.request === c.request;
const late = {} as const;
async function remove(kind: PresetType, keys: string[], request: number) {
  try { libraryRemoval.send({ type: 'REMOVED', request, undo: await removeRecords(kind === 'machine' ? 'printer' : kind, keys) }); }
  catch (e) { libraryRemoval.send({ type: 'FAIL', request, error: e instanceof Error ? e.message : String(e) }); }
}
async function restore(undo: RecordRemoval, request: number) {
  try { await restoreRecords(undo); libraryRemoval.send({ type: 'RESTORED', request }); }
  catch (e) { libraryRemoval.send({ type: 'FAIL', request, error: e instanceof Error ? e.message : String(e) }); }
}
const begin = { target: 'removing' as const, action: (c: RemovalContext, e: Extract<RemovalEvent, { type: 'REMOVE' }>) => { void remove(e.kind, e.keys, c.request + 1); return { request: c.request + 1, undo: null, error: null }; } };

export const libraryRemoval = createMachine<RemovalState, RemovalEvent, RemovalContext>({
  id: 'library-removal', initial: 'idle', strict: true, context: { request: 0, undo: null, error: null },
  states: {
    idle: { on: { REMOVE: begin, UNDO: {}, REMOVED: late, RESTORED: late, FAIL: late } },
    removing: { on: { REMOVE: {}, UNDO: {}, REMOVED: [{ target: 'removed', guard: mine, action: (_, e) => ({ undo: e.undo }) }, late],
      FAIL: [{ target: 'idle', guard: mine, action: (_, e) => ({ error: e.error }) }, late], RESTORED: late } },
    removed: { on: { REMOVE: begin, UNDO: { target: 'restoring', action: (c) => { void restore(c.undo!, c.request + 1); return { request: c.request + 1, error: null }; } }, REMOVED: late, RESTORED: late, FAIL: late } },
    restoring: { on: { REMOVE: {}, UNDO: {}, REMOVED: late, RESTORED: [{ target: 'idle', guard: mine, action: () => ({ undo: null }) }, late],
      FAIL: [{ target: 'removed', guard: mine, action: (_, e) => ({ error: e.error }) }, late] } },
  },
});

export function removeFromLibrary(kind: PresetType, keys: string[]): Promise<RecordRemoval> {
  return new Promise((resolve, reject) => {
    if (!libraryRemoval.can({ type: 'REMOVE', kind, keys })) { reject(new Error('Hang on — a removal or Undo is still wrapping up!')); return; }
    libraryRemoval.send({ type: 'REMOVE', kind, keys });
    const request = libraryRemoval.get().context.request;
    const stop = libraryRemoval.subscribe((s) => {
      if (s.context.request !== request || s.value === 'removing') return;
      stop();
      if (s.value === 'removed') resolve(s.context.undo!); else reject(new Error(s.context.error ?? 'Oops, couldn\'t remove that.'));
    });
  });
}
export function undoRemoval(undo: RecordRemoval): Promise<void> {
  return new Promise((resolve, reject) => {
    const { value, context } = libraryRemoval.get();
    if (value !== 'removed' || context.undo !== undo) { reject(new Error('Looks like that removal was already undone or replaced.')); return; }
    libraryRemoval.send({ type: 'UNDO' });
    const request = libraryRemoval.get().context.request;
    const stop = libraryRemoval.subscribe((s) => {
      if (s.context.request !== request || s.value === 'restoring') return;
      stop();
      if (s.value === 'idle') resolve(); else reject(new Error(s.context.error ?? 'Oops, couldn\'t undo that.'));
    });
  });
}
