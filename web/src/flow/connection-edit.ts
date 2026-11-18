import { createMachine } from './fsm';
import type { ConnectionTest, Discovered } from '../printers/connections';

export type ConnectionOp = 'test' | 'save' | 'find' | 'remove' | 'add' | 'adopt';
export type ConnectionEditState = 'idle' | 'testing' | 'saving' | 'finding' | 'removing' | 'adding' | 'adopting';
export interface ConnectionEditContext { request: number; error: string | null; test: ConnectionTest | null; found: { printers: Discovered[]; notes: string[] } | null }
type Result = Partial<Pick<ConnectionEditContext, 'test' | 'found'>> | void;
type ConnectionEditEvent = { type: 'RUN'; op: ConnectionOp; run(): Promise<Result> } | { type: 'DONE'; request: number; result: Result }
  | { type: 'FAIL'; request: number; error: string } | { type: 'EDIT' };

const STATE: Record<ConnectionOp, ConnectionEditState> = { test: 'testing', save: 'saving', find: 'finding', remove: 'removing', add: 'adding', adopt: 'adopting' };
const mine = (c: ConnectionEditContext, e: { request: number }) => e.request === c.request;
const late = {} as const;
const message = (e: unknown) => e instanceof Error ? e.message : String(e);

function create() {
  const runs = (Object.keys(STATE) as ConnectionOp[]).map((op) => ({ target: STATE[op], guard: (_: ConnectionEditContext, e: Extract<ConnectionEditEvent, { type: 'RUN' }>) => e.op === op,
    action: (c: ConnectionEditContext, e: Extract<ConnectionEditEvent, { type: 'RUN' }>, m: { send(e: ConnectionEditEvent): void }) => {
      const request = c.request + 1;
      e.run().then((result) => m.send({ type: 'DONE', request, result }), (err: unknown) => m.send({ type: 'FAIL', request, error: message(err) }));
      return { request, error: null, ...(op === 'test' ? { test: null } : {}) };
    } }));
  const edit = { action: () => ({ test: null }) } as const;
  const busy = { on: { RUN: late, EDIT: edit,
    DONE: [{ target: 'idle' as const, guard: mine, action: (_: ConnectionEditContext, e: Extract<ConnectionEditEvent, { type: 'DONE' }>) => e.result ?? {} }, late],
    FAIL: [{ target: 'idle' as const, guard: mine, action: (_: ConnectionEditContext, e: Extract<ConnectionEditEvent, { type: 'FAIL' }>) => ({ error: e.error }) }, late] } };
  return createMachine<ConnectionEditState, ConnectionEditEvent, ConnectionEditContext>({
    id: 'connection-edit', initial: 'idle', strict: true, context: { request: 0, error: null, test: null, found: null },
    states: {
      idle: { on: { RUN: runs, DONE: late, FAIL: late, EDIT: edit } },
      testing: { ...busy, on: { ...busy.on, EDIT: { target: 'idle', action: (c) => ({ request: c.request + 1, test: null }) } } },
      saving: busy, finding: busy, removing: busy, adding: busy, adopting: busy,
    },
  });
}
const machines = new Map<string, ReturnType<typeof create>>();
export const connectionEditFor = (printer: string) => {
  let m = machines.get(printer);
  if (!m) machines.set(printer, (m = create()));
  return m;
};
