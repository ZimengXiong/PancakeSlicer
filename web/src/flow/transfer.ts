import { mutation, responseError } from '../api/access';
import { getState, subscribeStore } from '../state/store';
import { assertNever, createMachine, useMachine } from './fsm';
import { readRecovery, rememberRecovery } from './recovery';
export type Transfer = { id: string; job: string; printer: string; name: string; kind: string; start: boolean; filename: string;
  state: 'uploading' | 'starting' | 'done' | 'failed' | 'cancelled'; sent: number; total: number; message?: string; error?: string;
  outcome?: 'uploaded' | 'printing' | 'unconfirmed' | 'failed';
  reason?: string; recovery?: string; started: string; finished?: string;
  job_end?: 'finished' | 'failed' | 'cancelled';
  job_errors?: { code: string; message?: string }[] };
export type TransferState = 'idle' | 'connecting' | 'sending' | 'sent' | 'failed';
type TransferEvent = { type: 'START'; jobId: string; target: string } | { type: 'STATUS'; transfer: Transfer }
  | { type: 'FAIL'; error: string } | { type: 'RESET' } | { type: 'CANCEL' } | { type: 'RETRY' };
interface TransferContext { jobId: string | null; target: string | null; transfer: Transfer | null; error: string }
const transferEffects = new WeakMap<object, { cancel(): void; retry(): void; reset(): void }>();
const reset = { target: 'idle', action: (_: TransferContext, _e: TransferEvent, m: object) => {
  transferEffects.get(m)?.reset(); return { jobId: null, target: null, transfer: null, error: '' };
} } as const;
const start = { target: 'connecting', guard: (_: TransferContext, e: Extract<TransferEvent, { type: 'START' }>) => !!e.jobId && !!e.target, action: (_: TransferContext, e: Extract<TransferEvent, { type: 'START' }>) =>
  ({ jobId: e.jobId, target: e.target, transfer: null, error: '' }) } as const;
function transferState(transfer: Transfer): 'sending' | 'sent' | 'failed' {
  const state = transfer.state;
  switch (state) {
    case 'uploading': case 'starting': return 'sending';
    case 'done': return 'sent';
    case 'failed': case 'cancelled': return 'failed';
    default: return assertNever(state);
  }
}
const status = (['sending', 'sent', 'failed'] as const).map((target) => ({ target,
  guard: (_: TransferContext, e: Extract<TransferEvent, { type: 'STATUS' }>) => transferState(e.transfer) === target,
  action: (_: TransferContext, e: Extract<TransferEvent, { type: 'STATUS' }>) => ({ transfer: e.transfer,
    error: transferState(e.transfer) === 'failed' ? e.transfer.error ?? e.transfer.message ?? 'Send failed' : '' }),
}));
const common = { RESET: reset, CANCEL: { action: (_: TransferContext, _e: TransferEvent, m: object) => { transferEffects.get(m)?.cancel(); } } };
const running = { ...common, STATUS: status, FAIL: { target: 'failed' as const, action: (_: TransferContext, e: { error: string }) => ({ error: e.error }) } };
export const createTransfer = () => createMachine<TransferState, TransferEvent, TransferContext>({
  id: 'transfer', initial: 'idle', context: { jobId: null, target: null, transfer: null, error: '' },
  states: {
    idle: { on: { START: start, RESET: reset } },
    connecting: { on: running }, sending: { on: running },
    sent: { on: { START: start, RESET: reset } },
    failed: { on: { START: start, RESET: reset, RETRY: { action: (_: TransferContext, _e: TransferEvent, m: object) => transferEffects.get(m)?.retry() } } },
  },
});
export function createPrinterSend() {
  const machine = createTransfer();
  let unfollow = () => {}, epoch = 0;
  const stop = () => { epoch++; unfollow(); unfollow = () => {}; };
  const follow = (jobId: string, target: string, id: string) => {
    const mine = epoch;
    let last = '', live = getState().printerConnections.live;
    unfollow = subscribeStore(() => {
      if (mine !== epoch) return;
      const c = getState().printerConnections, t = c.transfers.find((x) => x.id === id);
      if (c.live && !live) void connect(jobId, target);
      live = c.live;
      if (!t || JSON.stringify(t) === last) return;
      last = JSON.stringify(t);
      machine.send({ type: 'STATUS', transfer: t });
      if (machine.get().value !== 'sending') { unfollow(); if (machine.get().value === 'sent') rememberRecovery({ send: undefined }); }
    });
  };
  const connect = async (jobId: string, target: string, body?: Record<string, unknown>) => {
    if (body !== undefined && !machine.can({ type: 'START', jobId, target })) return;
    stop(); const mine = epoch;
    if (body !== undefined || machine.get().value !== 'sending') machine.send({ type: 'START', jobId, target });
    rememberRecovery({ send: { jobId, target } });
    try {
      const path = `/api/jobs/${encodeURIComponent(jobId)}/send`;
      const r = body !== undefined ? await mutation(path, { method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ ...body, printer: target }) }) : await fetch(path);
      if (body === undefined && r.status === 404) { if (mine === epoch) machine.send({ type: 'RESET' }); return; }
      if (!r.ok) throw await responseError(r);
      const transfer = await r.json() as Transfer;
      if (mine !== epoch) return;
      machine.send({ type: 'STATUS', transfer });
      if (machine.get().value === 'sending') follow(jobId, target, transfer.id);
      else if (machine.get().value === 'sent') rememberRecovery({ send: undefined });
    } catch (e) {
      if (mine === epoch) machine.send({ type: 'FAIL', error: e instanceof Error ? e.message : 'Oops! The connection dropped while sending your print. Reconnect to check if it went through.' });
    }
  };
  let retryRequest = Promise.resolve();
  transferEffects.set(machine, {
    cancel: () => {
      const { jobId } = machine.get().context, mine = epoch;
      if (jobId) void mutation(`/api/jobs/${encodeURIComponent(jobId)}/send/cancel`, { method: 'POST' }).then(async (r) => {
        if (!r.ok && r.status !== 409) { const error = (await responseError(r)).message; if (mine === epoch) machine.send({ type: 'FAIL', error }); }
      });
    },
    reset: () => { stop(); rememberRecovery({ send: undefined }); },
    retry: () => {
      const { jobId, target } = machine.get().context;
      if (jobId && target) retryRequest = connect(jobId, target);
    },
  });
  return { machine,
    submit: (printer: string, job: string, options: Record<string, unknown> = {}) => connect(job, printer, options),
    reconnect: (jobId: string, target: string) => connect(jobId, target),
    retry: () => { machine.send({ type: 'RETRY' }); return retryRequest; },
    cancel: () => machine.send({ type: 'CANCEL' }),
    reset: () => machine.send({ type: 'RESET' }),
  };
}
export const printerSend = createPrinterSend();
export function restorePrinterSend() {
  const send = readRecovery().send;
  if (send) void printerSend.reconnect(send.jobId, send.target);
}

export const usePrinterSend = () => useMachine(printerSend.machine);

export type SendSnapshot = { value: 'idle' } | { value: Exclude<TransferState, 'idle'>; jobId: string; target: string; transfer: Transfer | null; error: string };
export function sendSnapshot(machine = printerSend.machine): SendSnapshot {
  const { value, context: c } = machine.get();
  if (value === 'idle') return { value };
  if (!c.jobId || !c.target) throw new Error('Oops! Printer target details got out of sync.');
  return { value, jobId: c.jobId, target: c.target, transfer: c.transfer, error: c.error };
}
