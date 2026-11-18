import type { LibraryProfile } from '../profiles/identity';
import { creationPending, type ProfileCreation } from '../profiles/transactions';
import { createMachine, type StateNode } from './fsm';

export type WindowOp = 'adding' | 'copying' | 'exporting' | 'deleting' | 'editor-saving';
export type WindowOpsState = 'idle' | WindowOp;
export interface CopyRequest { source: LibraryProfile; name: string; creation: ProfileCreation; error?: string }
interface WindowOpsContext { copy: CopyRequest | null }
type WindowOpsEvent = { type: 'BEGIN'; op: WindowOp } | { type: 'END'; op: WindowOp }
  | { type: 'COPY'; source: LibraryProfile; name: string; creation: ProfileCreation } | { type: 'COPY_FAILED'; error: string }
  | { type: 'COPY_DONE' } | { type: 'DISCARD' };
type Node = StateNode<WindowOpsState, WindowOpsEvent, WindowOpsContext>;

const copyResult = {
  COPY_FAILED: { action: (c: WindowOpsContext, e: Extract<WindowOpsEvent, { type: 'COPY_FAILED' }>) => ({ copy: c.copy && { ...c.copy, error: e.error } }) },
  COPY_DONE: { action: () => ({ copy: null }) },
};
const op = (name: WindowOp): Node => ({ on: { END: { target: 'idle', guard: (_, e) => e.op === name }, ...(name === 'copying' ? copyResult : {}) } });

export const createWindowOps = () => createMachine<WindowOpsState, WindowOpsEvent, WindowOpsContext>({
  id: 'profiles-window-ops', initial: 'idle', strict: true, context: { copy: null },
  states: {
    idle: { on: {
      BEGIN: [{ target: 'adding', guard: (_, e) => e.op === 'adding' }, { target: 'copying', guard: (_, e) => e.op === 'copying' },
        { target: 'exporting', guard: (_, e) => e.op === 'exporting' }, { target: 'deleting', guard: (_, e) => e.op === 'deleting' },
        { target: 'editor-saving', guard: (_, e) => e.op === 'editor-saving' }],
      COPY: { action: (_, e) => ({ copy: { source: e.source, name: e.name, creation: e.creation } }) },
      DISCARD: { guard: (c) => !!c.copy && !creationPending(c.copy.creation), action: () => ({ copy: null }) },
    } },
    adding: op('adding'), copying: op('copying'), exporting: op('exporting'), deleting: op('deleting'), 'editor-saving': op('editor-saving'),
  },
});
export const windowOps = createWindowOps();
