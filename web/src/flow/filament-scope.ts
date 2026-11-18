import { createMachine, type Machine } from './fsm';

export type ScopeState = 'filament' | 'printer' | 'saving' | 'failed';
export interface PrinterSide { printer: string; extruder: number; label: string }
interface ScopeContext { side: PrinterSide | null; returned: boolean; request: number; error: string | null }
type ScopeEvent = { type: 'OPEN'; side: PrinterSide; dirty: boolean } | { type: 'BACK'; dirty: boolean } | { type: 'SAVE' }
  | { type: 'SAVED'; request: number } | { type: 'FAIL'; request: number; error: string } | { type: 'DISMISS' };
export type FilamentScope = Machine<ScopeState, ScopeEvent, ScopeContext>;

const mine = (c: ScopeContext, e: { request: number }) => e.request === c.request;
const late = {} as const;
const clean = (_: ScopeContext, e: { dirty: boolean }) => !e.dirty;
const open = { target: 'printer', guard: clean, action: (_: ScopeContext, e: { side: PrinterSide }) => ({ side: e.side, error: null }) } as const;
const back = { target: 'filament', guard: clean, action: () => ({ side: null, returned: true, error: null }) } as const;
const save = { target: 'saving', action: (c: ScopeContext) => ({ request: c.request + 1, error: null }) } as const;
const results = { SAVED: late, FAIL: late, DISMISS: late } as const;

const createFilamentScope = () => createMachine<ScopeState, ScopeEvent, ScopeContext>({
  id: 'filament-scope', initial: 'filament', strict: true, context: { side: null, returned: false, request: 0, error: null },
  states: {
    filament: { on: { ...results, OPEN: open, BACK: late } },
    printer: { on: { ...results, OPEN: open, BACK: back, SAVE: save } },
    saving: { on: { DISMISS: late, OPEN: late, BACK: late,
      SAVED: [{ target: 'printer', guard: mine }, late],
      FAIL: [{ target: 'failed', guard: mine, action: (_, e) => ({ error: e.error }) }, late] } },
    failed: { on: { ...results, OPEN: open, BACK: back, SAVE: save, DISMISS: { target: 'printer', action: () => ({ error: null }) } } },
  },
});

const scopes = new Map<string, FilamentScope>();
export function filamentScope(filament: string): FilamentScope {
  let m = scopes.get(filament);
  if (!m) scopes.set(filament, (m = createFilamentScope()));
  return m;
}
