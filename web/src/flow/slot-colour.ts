import { createMachine } from './fsm';
import { applySlotColour } from '../profiles/filament-colour';
import { toast } from '../state/store';

export type SlotColourState = 'idle' | 'writing' | 'failed';
interface SlotColourContext { request: number; error: string | null }
type SlotColourEvent = { type: 'APPLY'; colour: string } | { type: 'DONE'; request: number; note?: string }
  | { type: 'FAIL'; request: number; error: string } | { type: 'DISMISS' };

const mine = (c: SlotColourContext, e: { request: number }) => e.request === c.request;
const late = {} as const;

function create(printer: string, slot: number) {
  const apply = { target: 'writing', action: (c: SlotColourContext, e: Extract<SlotColourEvent, { type: 'APPLY' }>, m: { send(e: SlotColourEvent): void }) => {
    const request = c.request + 1;
    applySlotColour(slot, e.colour, printer).then((note) => m.send({ type: 'DONE', request, note }), (err: unknown) => m.send({ type: 'FAIL', request, error: err instanceof Error ? err.message : String(err) }));
    return { request, error: null };
  } } as const;
  return createMachine<SlotColourState, SlotColourEvent, SlotColourContext>({
    id: 'slot-colour', initial: 'idle', strict: true, context: { request: 0, error: null },
    states: {
      idle: { on: { APPLY: apply, DONE: late, FAIL: late, DISMISS: late } },
      writing: { on: { APPLY: late, DISMISS: late,
        DONE: [{ target: 'idle', guard: mine, action: (_, e) => { if (e.note) toast('slot_colour_note', { note: e.note }); } }, late],
        FAIL: [{ target: 'failed', guard: mine, action: (_, e) => ({ error: e.error }) }, late] } },
      failed: { on: { APPLY: apply, DONE: late, FAIL: late, DISMISS: { target: 'idle', action: () => ({ error: null }) } } },
    },
  });
}
const machines = new Map<string, ReturnType<typeof create>>();
export const slotColourFor = (printer: string, slot: number) => {
  const key = `${printer}\n${slot}`;
  let m = machines.get(key);
  if (!m) machines.set(key, (m = create(printer, slot)));
  return m;
};
