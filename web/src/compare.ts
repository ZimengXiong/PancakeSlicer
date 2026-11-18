import type { EngineWarning } from './engine-warnings';
import { useSyncExternalStore } from 'react';
import { jobRequest } from './actions';
import type { JobStatus } from './api/types';
import type { PreviewData } from './preview/data';
import { jobValue } from './options';
import { getState } from './state/store';
import { createMachine, type StateNode } from './flow/fsm';

export type CompareSource = 'standard' | 'previous';
export type CompareState =
  | { state: 'idle' }
  | { state: 'running'; source: 'standard'; key: string; queued: boolean; pct: number }
  | { state: 'done'; source: CompareSource; key: string; id: string; status: JobStatus }
  | { state: 'failed'; source: 'standard'; key: string; error: string; busy: boolean };

type CompareEvent = { type: 'START'; key: string } | { type: 'SHOW'; source: CompareSource; key: string; id: string; status: JobStatus } | { type: 'STOP' }
  | { type: 'FORGET'; key: string } | { type: 'PROGRESS'; request: number; queued: boolean; pct: number }
  | { type: 'DONE'; request: number; id: string; status: JobStatus } | { type: 'FAIL'; request: number; error: string; busy: boolean };
interface CompareContext { request: number; view: CompareState }
const cache = new Map<string, { id: string; status: JobStatus }>();
let active: { id?: string; stop?: () => void; submission: AbortController } | null = null;
function stopActive() {
  const a = active;
  active = null;
  a?.submission.abort();
  a?.stop?.();
  if (a?.id) void getState().api?.cancel(a.id).catch(() => undefined);
}
type Ctx = CompareContext;
type On = NonNullable<StateNode<CompareState['state'], CompareEvent, Ctx>['on']>;
const listeners = new Set<() => void>();
const mine = (c: Ctx, e: { request: number }) => e.request === c.request;
const key = (c: Ctx) => c.view.state === 'idle' ? '' : c.view.key;
const start: On['START'] = { target: 'running', action: (c, e) => ({ request: c.request + 1, view: { state: 'running', source: 'standard', key: e.key, queued: true, pct: 0 } }) };
const show: On['SHOW'] = { target: 'done', action: (c, e) => ({ request: c.request + 1, view: { state: 'done', source: e.source, key: e.key, id: e.id, status: e.status } }) };
const stopped = (c: Ctx): Partial<Ctx> => ({ request: c.request + 1, view: { state: 'idle' } });
const late = {};
const settled: On = { START: start, SHOW: show, STOP: { target: 'idle', action: stopped },
  FORGET: [{ target: 'idle', guard: (c, e) => key(c) === e.key, action: stopped }, late], PROGRESS: late, DONE: late, FAIL: late };
const compare = createMachine<CompareState['state'], CompareEvent, CompareContext>({
  id: 'compare', initial: 'idle', strict: true, context: { request: 0, view: { state: 'idle' } },
  states: {
    idle: { on: { ...settled, STOP: {} } },
    running: { exit: () => { stopActive(); }, on: { ...settled,
      PROGRESS: [{ guard: mine, action: (c, e) => ({ view: { state: 'running', source: 'standard', key: key(c), queued: e.queued, pct: e.pct } }) }, late],
      DONE: [{ target: 'done', guard: mine, action: (c, e) => { cache.set(key(c), { id: e.id, status: e.status }); return { view: { state: 'done', source: 'standard', key: key(c), id: e.id, status: e.status } }; } }, late],
      FAIL: [{ target: 'failed', guard: mine, action: (c, e) => ({ view: { state: 'failed', source: 'standard', key: key(c), error: e.error, busy: e.busy } }) }, late],
    } },
    done: { on: settled },
    failed: { on: settled },
  },
});
export const compareMachine = compare;

const subscribe = (l: () => void) => (listeners.add(l), () => void listeners.delete(l));
export function useCompare(): CompareState {
  return useSyncExternalStore(compare.subscribe, () => compare.get().context.view);
}

let wanted: CompareSource | null = null;
export function setCompareWanted(source: CompareSource | null) {
  wanted = source;
  if (!source) baseline = null;
  for (const l of listeners) l();
}
export function useCompareWanted(): CompareSource | null {
  return useSyncExternalStore(subscribe, () => wanted);
}

export interface Baseline { id: string; status: JobStatus; signature: string; warnings: EngineWarning[] }
let baseline: Baseline | null = null;
export function useBaseline(): Baseline | null {
  return useSyncExternalStore(subscribe, () => baseline);
}

export function sliceAndCompare(slice: () => Promise<unknown>) {
  const job = getState().job;
  if (job.state !== 'done' || !job.id || !job.status || !job.plateSignature) return;
  baseline = { id: job.id, status: job.status, signature: job.plateSignature, warnings: job.warnings ?? [] };
  setCompareWanted('previous');
  void slice();
}

export function showPrevious() {
  if (baseline) compare.send({ type: 'SHOW', source: 'previous', key: baseline.signature, id: baseline.id, status: baseline.status });
}

let standardData: PreviewData | null = null;
export function setStandardData(d: PreviewData | null) {
  standardData = d;
  for (const l of listeners) l();
}
export function useStandardData(): PreviewData | null {
  return useSyncExternalStore(subscribe, () => standardData);
}

async function standardRequest() {
  const req = await jobRequest(getState());
  const flat = { nonplanar_mode: jobValue('nonplanar_mode', 'off'), zaa_enabled: false };
  return {
    ...req,
    project: { ...req.project, process: { ...req.project.process, ...flat } },
    plate: req.plate && { objects: req.plate.objects.map((o) => {
      const { nonplanar_mode: _, ...rest } = o.settings ?? {};
      return { ...o, settings: { ...rest, zaa_enabled: '0' } };
    }) },
    project_settings: req.plate ? undefined : { zaa_enabled: false },
    force_planar: true,
    output: 'gcode' as const,
    priority: 'low' as const,
  };
}

export async function startCompare() {
  const s = getState();
  const key = s.job.plateSignature;
  if (!s.api || s.job.state !== 'done' || !key) return;
  const kept = cache.get(key);
  if (kept) return compare.send({ type: 'SHOW', source: 'standard', key, ...kept });
  const now = compare.get().context.view;
  if (now.state === 'running' && now.key === key) return;
  compare.send({ type: 'START', key });
  const request = compare.get().context.request, submission = new AbortController(), own = { submission } as NonNullable<typeof active>;
  active = own;
  const current = () => compare.get().context.request === request;
  try {
    const id = await s.api.submit(await standardRequest(), submission.signal);
    if (!current()) return void s.api.cancel(id).catch(() => undefined);
    own.id = id;
    own.stop = s.api.events(id, {
      progress: (e) => compare.send({ type: 'PROGRESS', request, queued: e.stage === 'queued', pct: Math.max(0, Math.min(100, e.pct)) }),
      done: (status) => { if (current()) active = null; compare.send({ type: 'DONE', request, id, status }); },
      error: (error) => { if (current()) active = null; compare.send({ type: 'FAIL', request, error, busy: false }); },
    });
  } catch (e) {
    if (current()) active = null;
    const status = (e as { status?: number }).status;
    compare.send({ type: 'FAIL', request, error: String((e as Error).message ?? e), busy: status === 429 || status === 503 });
  }
}

export const stopCompare = () => compare.send({ type: 'STOP' });

export function forgetCompare(key: string) {
  cache.delete(key);
  compare.send({ type: 'FORGET', key });
}
