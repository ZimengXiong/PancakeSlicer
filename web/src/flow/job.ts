import type { Api } from '../api/client';
import type { JobStatus, ProgressEvent } from '../api/types';
import { mutation, responseError } from '../api/access';
import { followJobStream } from '../api/job-stream';
import { assertNever, createMachine, type StateNode } from './fsm';

export function waitForJob(api: Api, id: string, signal?: AbortSignal, progress?: (p: ProgressEvent) => void): Promise<void> {
  return new Promise((resolve, reject) => {
    let settled = false, unsubscribe = () => {}, timer: ReturnType<typeof setTimeout> | undefined;
    const finish = (error?: unknown) => {
      if (settled) return;
      settled = true; unsubscribe(); clearTimeout(timer); clearTimeout(deadline);
      signal?.removeEventListener('abort', abort);
      if (error) reject(error); else resolve();
    };
    const abort = () => {
      void api.cancel(id).catch(() => {});
      finish(signal?.reason ?? new DOMException('Canceled', 'AbortError'));
    };
    const deadline = setTimeout(() => {
      void api.cancel(id).catch(() => {});
      finish(new Error('Oops! Exporting the project took too long and timed out. Make sure the slicing service is running and try again.'));
    }, 5 * 60_000);
    const poll = async () => {
      let timeout: ReturnType<typeof setTimeout> | undefined;
      try {
        const status = await Promise.race([api.status(id), new Promise<never>((_, fail) => {
          timeout = setTimeout(() => fail(new Error('Oops! The slicing service stopped responding while exporting. Please try exporting again.')), 10_000);
        })]);
        if (settled) return;
        if (jobLifecycle(status as CanonicalJobStatus) === 'done') finish();
        else if (jobTerminal(jobLifecycle(status as CanonicalJobStatus))) finish(new Error(status.error ?? `Project export ${jobLifecycle(status as CanonicalJobStatus)}`));
        else timer = setTimeout(() => void poll(), 5000);
      } catch (e) { finish(e); }
      finally { clearTimeout(timeout); }
    };
    signal?.addEventListener('abort', abort, { once: true });
    if (signal?.aborted) { abort(); return; }
    try {
      unsubscribe = api.events(id, { progress: (p) => { if (!settled) progress?.(p); }, done: () => finish(), error: (message) => finish(new Error(message)) });
      if (settled) unsubscribe();
      else timer = setTimeout(() => void poll(), 5000);
    } catch (e) { finish(e); }
  });
}

export type JobLifecycle = 'idle' | 'queued' | 'preparing' | 'slicing' | 'progress' | 'storing' | 'done' | 'failed'
  | 'canceling' | 'cancelled' | 'interrupted' | 'stale';
export type CanonicalJobStatus = JobStatus & { lifecycle_state?: Exclude<JobLifecycle, 'progress'>; seq?: number };
export function jobLifecycle(status: CanonicalJobStatus): Exclude<JobLifecycle, 'progress'> {
  if (status.lifecycle_state && Object.hasOwn(jobStates, status.lifecycle_state)) return status.lifecycle_state;
  const state = status.state;
  switch (state) {
    case 'queued': case 'done': case 'failed': return state;
    case 'running': return 'slicing';
    case 'canceled': return 'cancelled';
    default: return assertNever(state);
  }
}
export const jobTerminal = (state: JobLifecycle) => ['done', 'failed', 'cancelled', 'interrupted', 'stale'].includes(state);
export interface TrackedJob { id: string; projectId: string; signature: string }
interface JobContext { job: TrackedJob | null; seq: number; pct: number; stage: string; error: string | null; status: CanonicalJobStatus | null }
type JobEvent = { type: 'TRACK'; job: TrackedJob; seq?: number } | { type: 'STATUS'; status: CanonicalJobStatus }
  | { type: 'PROGRESS'; id: string; seq: number; stage: string; pct: number }
  | { type: 'CANCEL_ACK'; status: CanonicalJobStatus }
  | { type: 'DISCONNECT'; id: string } | { type: 'CANCEL' } | { type: 'RETRY' } | { type: 'RESET' };
const track = { target: 'queued', action: (_: JobContext, e: Extract<JobEvent, { type: 'TRACK' }>) =>
  ({ job: e.job, seq: e.seq ?? -1, pct: 0, stage: 'queued', error: null, status: null }) } as const;
const reset = { target: 'idle', action: () => ({ job: null, seq: -1, pct: 0, stage: '', error: null, status: null }) } as const;
const statusEdges = (allowed: Exclude<JobLifecycle, 'progress'>[]): { target: JobLifecycle; guard: (c: JobContext, e: { status: CanonicalJobStatus }) => boolean;
    action: (c: JobContext, e: { status: CanonicalJobStatus }) => Partial<JobContext> }[] =>
  allowed.map((target) => ({ target, guard: (c, e) => e.status.id === c.job?.id && jobLifecycle(e.status) === target,
    action: (c, e) => ({ status: e.status, pct: e.status.progress ?? c.pct, stage: e.status.stage ?? target, error: e.status.error ?? null }) }));
const progress = { target: 'progress', guard: (c: JobContext, e: Extract<JobEvent, { type: 'PROGRESS' }>) =>
  c.job?.id === e.id && e.seq > c.seq, action: (_: JobContext, e: Extract<JobEvent, { type: 'PROGRESS' }>) =>
    ({ seq: e.seq, pct: e.pct, stage: e.stage }) } as const;
const jobEffects = new WeakMap<object, { cancel(): void; retry(): void }>();
const cancel = { target: 'canceling', action: (_: JobContext, _e: JobEvent, m: object) => jobEffects.get(m)?.cancel() } as const;
const disconnect = { target: 'interrupted', guard: (c: JobContext, e: Extract<JobEvent, { type: 'DISCONNECT' }>) => c.job?.id === e.id,
  action: () => ({ error: 'Oops! Connection dropped. Click Retry to reconnect or restart the job.' }) } as const;
const active = { TRACK: track, RESET: reset, CANCEL: cancel, DISCONNECT: disconnect };
const settled = { TRACK: track, RESET: reset, RETRY: { action: (_: JobContext, _e: JobEvent, m: object) => jobEffects.get(m)?.retry() } };
const jobStates: Record<JobLifecycle, StateNode<JobLifecycle, JobEvent, JobContext>> = {
  idle: { on: { TRACK: track, RESET: reset } },
  queued: { on: { ...active, STATUS: statusEdges(['queued', 'preparing', 'slicing', 'storing', 'done', 'failed', 'canceling', 'cancelled', 'interrupted', 'stale']), PROGRESS: progress } },
  preparing: { on: { ...active, STATUS: statusEdges(['preparing', 'slicing', 'storing', 'done', 'failed', 'canceling', 'cancelled', 'interrupted', 'stale']), PROGRESS: progress } },
  slicing: { on: { ...active, STATUS: statusEdges(['slicing', 'storing', 'done', 'failed', 'canceling', 'cancelled', 'interrupted', 'stale']), PROGRESS: progress } },
  progress: { on: { ...active, STATUS: statusEdges(['slicing', 'storing', 'done', 'failed', 'canceling', 'cancelled', 'interrupted', 'stale']), PROGRESS: progress } },
  storing: { on: { ...active, STATUS: statusEdges(['storing', 'done', 'failed', 'canceling', 'cancelled', 'interrupted', 'stale']) } },
  canceling: { on: { RESET: reset, TRACK: track, CANCEL: {}, DISCONNECT: disconnect, STATUS: statusEdges(['canceling', 'cancelled', 'interrupted']),
    CANCEL_ACK: statusEdges(['canceling', 'cancelled', 'interrupted', 'done', 'failed', 'stale']) } },
  done: { on: { RESET: reset, TRACK: track, STATUS: statusEdges(['stale']) } },
  failed: { on: settled }, cancelled: { on: settled }, stale: { on: settled }, interrupted: { on: settled },
};
export const createJob = () => createMachine<JobLifecycle, JobEvent, JobContext>({
  id: 'job', initial: 'idle', context: { job: null, seq: -1, pct: 0, stage: '', error: null, status: null }, states: jobStates,
});
export const job = createJob();
type JobIdentity = { job: TrackedJob; seq: number; pct: number; stage: string };
export type JobSnapshot = { value: 'idle' } | (JobIdentity & (
  { value: 'failed' | 'interrupted'; error: string }
  | { value: Exclude<JobLifecycle, 'idle' | 'failed' | 'interrupted'>; error: string | null }));
export function jobSnapshot(machine = job): JobSnapshot {
  const { value, context: c } = machine.get();
  if (value === 'idle') return { value };
  if (!c.job) throw new Error('Oops! Slice job state got out of sync.');
  const identity = { job: c.job, seq: c.seq, pct: c.pct, stage: c.stage };
  return value === 'failed' || value === 'interrupted'
    ? { ...identity, value, error: c.error ?? `Job ${value}. Click Retry to try again.` }
    : { ...identity, value, error: c.error };
}

export function readJobEvents(id: string, seq: number, signal: AbortSignal,
  receive: (data: Record<string, unknown>, seq: number) => void): Promise<void> {
  return new Promise((resolve, reject) => {
    if (signal.aborted) return resolve();
    const abort = () => { stop(); resolve(); };
    const stop = followJobStream(id, seq, (e) => {
      let event: Record<string, unknown>;
      try { event = JSON.parse(e.data); } catch { return; }
      receive(event, e.seq);
    }, (error) => { signal.removeEventListener('abort', abort); if (error) reject(error); else resolve(); });
    signal.addEventListener('abort', abort, { once: true });
  });
}

export function followJob(api: Api, tracked: TrackedJob, options: {
  machine?: ReturnType<typeof createJob>; seq?: number;
  progress?: (p: ProgressEvent) => void; done?: (status: CanonicalJobStatus) => void;
  failed?: (message: string) => void; checkpoint?: (seq: number, stage: string) => void;
} = {}) {
  const m = options.machine ?? createJob();
  let streamTimer: ReturnType<typeof setTimeout> | undefined;
  let retrying = false;
  let ctl = new AbortController(), timer: ReturnType<typeof setTimeout> | undefined, current = tracked, stopped = false, last = options.seq ?? -1;
  m.send({ type: 'TRACK', job: current, seq: last });
  const alive = () => !stopped && !ctl.signal.aborted;
  const apply = (status: CanonicalJobStatus) => {
    if (!alive() || status.id !== current.id) return;
    m.send({ type: 'STATUS', status });
    options.checkpoint?.(last, m.get().value);
    if (jobTerminal(m.get().value)) {
      if (m.get().value === 'done') options.done?.(status);
      else options.failed?.(status.error ?? `Job ${m.get().value}. Click Retry to try again.`);
      ctl.abort(); clearTimeout(timer); clearTimeout(streamTimer);
    }
  };
  const status = async () => {
    const signal = ctl.signal, id = current.id;
    let timeout: ReturnType<typeof setTimeout> | undefined;
    try {
      const snapshot = await Promise.race([api.status(id), new Promise<never>((_, reject) => {
        timeout = setTimeout(() => reject(new Error('Oops! The slicing service stopped responding.')), 10_000);
      })]) as CanonicalJobStatus;
      if (signal === ctl.signal && !signal.aborted) apply(snapshot);
    } finally { clearTimeout(timeout); }
  };
  const poll = async () => {
    if (!alive()) return;
    const owner = ctl;
    try { await status(); } catch {
      if (owner === ctl && alive()) { m.send({ type: 'DISCONNECT', id: current.id }); options.failed?.('Connection dropped—click Retry to reconnect.'); ctl.abort(); }
    }
    if (owner === ctl && alive()) timer = setTimeout(() => void poll(), 5000);
  };
  const stream = async () => {
    const id = current.id, owner = ctl;
    try {
      await readJobEvents(id, last, owner.signal, (data, seq) => {
        if (owner !== ctl || !alive() || current.id !== id) return;
        last = seq;
        if (typeof data.pct === 'number') {
          const stage = String(data.stage ?? 'slicing');
          const event = { type: 'PROGRESS', id, seq, pct: data.pct, stage } as const;
          const accepted = m.can(event);
          m.send(event);
          if (accepted) options.progress?.({ pct: data.pct, stage });
        }
        options.checkpoint?.(last, m.get().value);
        if (typeof data.state === 'string' || typeof data.lifecycle_state === 'string') {
          if (data.lifecycle_state === 'done' || data.state === 'done') void status().catch(() => {});
          else apply({ ...data, id } as CanonicalJobStatus);
        }
      });
    } catch {  }
    if (owner === ctl && alive()) streamTimer = setTimeout(() => void stream(), 1000);
  };
  const connect = async () => {
    const owner = ctl;
    try { await status(); } catch {
      if (owner === ctl && alive()) { m.send({ type: 'DISCONNECT', id: current.id }); options.failed?.('Connection dropped—click Retry to reconnect.'); ctl.abort(); }
    }
    if (owner === ctl && alive()) { timer = setTimeout(() => void poll(), 5000); void stream(); }
  };
  const performCancel = async () => {
    try {
      if (api.mock) await api.cancel(current.id);
      else {
        const r = await mutation(`/api/jobs/${encodeURIComponent(current.id)}/cancel`, { method: 'POST' });
        const acknowledged = await r.json() as CanonicalJobStatus;
        if (alive() && acknowledged.id === current.id) {
          m.send({ type: 'CANCEL_ACK', status: acknowledged });
          if (jobTerminal(m.get().value)) apply(acknowledged);
        }
      }
      if (alive()) await status();
    } catch (e) {
      if (alive()) m.send({ type: 'DISCONNECT', id: current.id });
      throw e;
    }
  };
  const performRetry = async () => {
    if (retrying) return;
    retrying = true;
    try {
      ctl.abort(); clearTimeout(timer); clearTimeout(streamTimer);
      let existing: CanonicalJobStatus | null = null;
      let timeout: ReturnType<typeof setTimeout> | undefined;
      try {
        existing = await Promise.race([api.status(current.id), new Promise<never>((_, reject) => {
          timeout = setTimeout(() => reject(new Error('Oops! The slicing service stopped responding.')), 10_000);
        })]) as CanonicalJobStatus;
      } catch {  }
      finally { clearTimeout(timeout); }
      if (stopped) return;
      if (existing && jobTerminal(jobLifecycle(existing))) {
        const r = await mutation(`/api/jobs/${encodeURIComponent(current.id)}/retry`, {
          method: 'POST', headers: { 'Idempotency-Key': `retry-${current.id}` },
        });
        if (!r.ok) throw await responseError(r);
        const next = await r.json() as { id: string };
        if (stopped) return;
        current = { ...current, id: next.id }; last = -1;
      }
      ctl = new AbortController();
      m.send({ type: 'TRACK', job: current, seq: last });
      options.checkpoint?.(last, m.get().value);
      void connect();
    } finally { retrying = false; }
  };
  let cancellation = Promise.resolve(), retryRequest = Promise.resolve();
  jobEffects.set(m, {
    cancel: () => { cancellation = performCancel(); void cancellation.catch(() => {}); },
    retry: () => { retryRequest = performRetry(); void retryRequest.catch(() => {}); },
  });
  options.checkpoint?.(last, m.get().value);
  void connect();
  return { machine: m,
    stop() { stopped = true; ctl.abort(); clearTimeout(timer); clearTimeout(streamTimer); jobEffects.delete(m); },
    cancel() { m.send({ type: 'CANCEL' }); return cancellation; },
    retry() { m.send({ type: 'RETRY' }); return retryRequest; },
  };
}
