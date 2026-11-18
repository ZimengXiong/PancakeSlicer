import { createMachine, type Machine } from './fsm';

export type NozzleSizeState = 'idle' | 'entering' | 'saving' | 'failed';
interface NozzleSizeContext { text: string; error: string | null; request: number }
type NozzleSizeEvent = { type: 'OTHER'; text: string } | { type: 'TYPE'; text: string } | { type: 'CANCEL' } | { type: 'SAVE' }
  | { type: 'SAVED'; request: number } | { type: 'FAIL'; request: number; error: string } | { type: 'DISMISS' };
export type NozzleSize = Machine<NozzleSizeState, NozzleSizeEvent, NozzleSizeContext>;

const mine = (c: NozzleSizeContext, e: { request: number }) => e.request === c.request;
const late = {} as const;
const save = { target: 'saving', action: (c: NozzleSizeContext) => ({ request: c.request + 1, error: null }) } as const;
const createNozzleSize = () => createMachine<NozzleSizeState, NozzleSizeEvent, NozzleSizeContext>({
  id: 'nozzle-size', initial: 'idle', strict: true, context: { text: '', error: null, request: 0 },
  states: {
    idle: { on: { OTHER: { target: 'entering', action: (_, e) => ({ text: e.text, error: null }) }, SAVE: save, SAVED: late, FAIL: late, DISMISS: late, TYPE: late, CANCEL: late } },
    entering: { on: { TYPE: { action: (_, e) => ({ text: e.text }) }, CANCEL: { target: 'idle' }, SAVE: save, OTHER: late, SAVED: late, FAIL: late, DISMISS: late } },
    saving: { on: { SAVED: [{ target: 'idle', guard: mine }, late], FAIL: [{ target: 'failed', guard: mine, action: (_, e) => ({ error: e.error }) }, late],
      OTHER: late, TYPE: late, CANCEL: late, SAVE: late, DISMISS: late } },
    failed: { on: { DISMISS: { target: 'idle', action: () => ({ error: null }) }, OTHER: { target: 'entering', action: (_, e) => ({ text: e.text, error: null }) }, SAVE: save,
      SAVED: late, FAIL: late, TYPE: late, CANCEL: late } },
  },
});
const sizes = new Map<string, NozzleSize>();
export function nozzleSize(printer: string): NozzleSize {
  const key = printer;
  let m = sizes.get(key);
  if (!m) sizes.set(key, (m = createNozzleSize()));
  return m;
}
