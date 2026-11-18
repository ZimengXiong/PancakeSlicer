import { createMachine, useMachine, type Machine } from '../flow/fsm';
import { failureText, type ApiError } from '../api/access';
import { controlDevice } from './connections';

export type DeviceControl = 'nozzle' | 'bed' | 'chamber' | 'fan' | 'speed' | 'light';
export interface DeviceCommand { control: DeviceControl; fan?: string; extruder?: number }

type ControlState = 'idle' | 'queued' | 'pending' | 'sending' | 'retrying' | 'applied' | 'failed';
interface ControlContext { value: number | null; next: number | null; error: string | null; request: number }
type ControlEvent = { type: 'COMMIT'; value: number } | { type: 'START'; request: number } | { type: 'DONE'; request: number }
  | { type: 'RETRY'; request: number } | { type: 'FAIL'; request: number; error: string };
type ControlMachine = Machine<ControlState, ControlEvent, ControlContext>;

const SETTLE_MS = 250;
const RETRY_WINDOW_MS = 30_000;

const queues = new Map<string, Promise<void>>();
function enqueue(printer: string, work: () => Promise<void>) {
  const run = (queues.get(printer) ?? Promise.resolve()).then(work, work);
  queues.set(printer, run.catch(() => undefined));
}

const mine = (c: ControlContext, e: { request: number }) => e.request === c.request;
const late = {} as const;
function create(printer: string, command: DeviceCommand): ControlMachine {
  const send = (request: number) => enqueue(printer, async () => {
    const deadline = Date.now() + RETRY_WINDOW_MS;
    for (;;) {
      m.send({ type: 'START', request });
      const { value: state, context } = m.get();
      if (state !== 'sending' || !mine(context, { request }) || context.value === null) return;
      try {
        await controlDevice(printer, { ...command, value: context.value });
        m.send({ type: 'DONE', request });
        return;
      } catch (e) {
        const err = e as Partial<ApiError> & { retryAfter?: number };
        const wait = (err.retryAfter ?? 1) * 1000;
        if (err.status !== 429 || Date.now() + wait > deadline) {
          const error = err.status === 429 ? 'Whoa, too many commands at once! This one didn\'t go through — give it a second and try again.'
            : e instanceof TypeError ? failureText('Not sent', e)
            : err.code?.startsWith('control_') ? err.message ?? 'The printer didn\'t confirm that.' : `Oops, the printer didn't like that: ${err.message}`;
          m.send({ type: 'FAIL', request, error });
          return;
        }
        m.send({ type: 'RETRY', request });
        await new Promise((resolve) => setTimeout(resolve, wait));
      }
    }
  });
  const queue = { target: 'queued' as const, action: (c: ControlContext, e: { value: number }) => ({ value: e.value, next: null, error: null, request: c.request + 1 }) };
  const replace = { action: (_: ControlContext, e: { value: number }) => ({ value: e.value }) };
  const m: ControlMachine = createMachine<ControlState, ControlEvent, ControlContext>({
    id: 'device-control', initial: 'idle', strict: true, context: { value: null, next: null, error: null, request: 0 },
    states: {
      idle: { on: { COMMIT: queue, START: late, DONE: late, RETRY: late, FAIL: late } },
      queued: { after: { ms: SETTLE_MS, target: 'pending', action: (c) => { send(c.request); } },
        on: { COMMIT: { target: 'queued', action: (_, e) => ({ value: e.value }) }, START: late, DONE: late, RETRY: late, FAIL: late } },
      pending: { on: { COMMIT: replace, START: [{ target: 'sending', guard: mine }, late], DONE: late, RETRY: late, FAIL: late } },
      sending: { on: { COMMIT: { action: (_, e) => ({ next: e.value }) },
        DONE: [{ target: 'queued', guard: (c, e) => mine(c, e) && c.next !== null, action: (c) => ({ value: c.next, next: null, request: c.request + 1 }) },
          { target: 'applied', guard: mine, action: () => ({ value: null }) }, late],
        RETRY: [{ target: 'retrying', guard: mine, action: (c) => ({ value: c.next ?? c.value, next: null }) }, late],
        FAIL: [{ target: 'failed', guard: mine, action: (_, e) => ({ value: null, next: null, error: e.error }) }, late],
        START: late } },
      retrying: { on: { COMMIT: replace, START: [{ target: 'sending', guard: mine }, late], DONE: late, RETRY: late, FAIL: late } },
      applied: { on: { COMMIT: queue, START: late, DONE: late, RETRY: late, FAIL: late } },
      failed: { on: { COMMIT: queue, START: late, DONE: late, RETRY: late, FAIL: late } },
    },
  });
  return m;
}

const machines = new Map<string, ControlMachine>();
const keyOf = (printer: string, c: DeviceCommand) => `${printer}\n${c.control}\n${c.fan ?? ''}\n${c.extruder ?? ''}`;
function machineOf(printer: string, command: DeviceCommand): ControlMachine {
  const key = keyOf(printer, command);
  let m = machines.get(key);
  if (!m) machines.set(key, (m = create(printer, command)));
  return m;
}

export function useDeviceControl(printer: string, command: DeviceCommand, reported: number | undefined) {
  const m = machineOf(printer, command), snap = useMachine(m);
  const inFlight = snap.value === 'queued' || snap.value === 'pending' || snap.value === 'sending' || snap.value === 'retrying';
  return {
    state: snap.value,
    value: inFlight ? (snap.context.next ?? snap.context.value ?? reported) : reported,
    error: snap.value === 'failed' ? snap.context.error : snap.value === 'retrying' ? 'Printer\'s a bit busy — we\'ll try sending again in a sec…' : null,
    commit: (value: number) => m.send({ type: 'COMMIT', value }),
  };
}
