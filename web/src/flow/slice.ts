import { warningKey, type EngineWarning } from '../engine-warnings';
import { plateSignature } from '../slice-input';
import { documentGeneration, subscribeHistory } from '../state/history';
import { IDLE_JOB, getState, setState, subscribeStore, type JobView, type State } from '../state/store';
import { createMachine, useMachine, type Snapshot } from './fsm';

export interface SliceSummary {
  timeS: number | null;
  grams: number | null;
  supportG: number | null;
  layers: number | null;
  warnings: EngineWarning[];
}

export interface SliceResult {
  signature: string;
  job: JobView;
  summary: SliceSummary;
  at: number;
}

export interface SliceDiff {
  timeS: number | null;
  grams: number | null;
  supportG: number | null;
  layers: number | null;
  supportGone: boolean;
  supportAdded: boolean;
  warningsAdded: EngineWarning[];
  warningsCleared: EngineWarning[];
}

export type SliceStateName = 'editing' | 'slicing' | 'sliced' | 'stale' | 'failed';

export interface SliceContext {
  result: SliceResult | null;
  previous: SliceResult | null;
  target: string | null;
  pct: number;
  stage: string;
  error: string | null;
}

export type SliceEvent =
  | { type: 'CHANGE' }
  | { type: 'RESET' }
  | { type: 'CANCEL' }
  | { type: 'START'; signature: string }
  | { type: 'RESTORE'; result: SliceResult }
  | { type: 'PROGRESS'; pct: number; stage: string }
  | { type: 'ERROR'; error: string }
  | { type: 'ANNOTATE'; signature: string; part: Partial<SliceSummary> };

const CACHE_SIZE = 12;

const num = (v: unknown) => (typeof v === 'number' && Number.isFinite(v) ? v : typeof v === 'string' && v.trim() && Number.isFinite(+v) ? +v : null);

export function summarize(job: JobView): SliceSummary {
  const r = (job.status?.record ?? {}) as Record<string, Record<string, unknown> | undefined>;
  return {
    timeS: num(r.totals?.estimated_time_s) ?? num(r.text_summary?.estimated_time_s),
    grams: num(r.statistics?.total_weight_g),
    supportG: num(r.statistics?.support_weight_g),
    layers: num(r.totals?.layer_count) ?? num(r.statistics?.layer_count),
    warnings: job.warnings ?? [],
  };
}

const cache = new Map<string, SliceResult>();
function keep(r: SliceResult) {
  cache.delete(r.signature);
  cache.set(r.signature, r);
  for (const [k, old] of cache) {
    if (cache.size <= CACHE_SIZE) break;
    cache.delete(k);
    const ctx = slice.get().context;
    if (old.job.id && old !== ctx.result && old !== ctx.previous) void getState().api?.cancel(old.job.id).catch(() => undefined);
  }
}

const ALWAYS: (keyof State)[] = ['objects', 'projectPlates', 'plateIndex', 'project', 'events', 'records',
  'printer', 'process', 'filamentEdits', 'wipeTowerPos'];
const NEVER = new Set<keyof State>(['job', 'tab', 'selected', 'toast', 'toastSeq', 'toastAction', 'toastMs']);
let watched: (keyof State)[] = ALWAYS;
let seen: State = getState();

function signature(s = getState()): string {
  const read = new Set<keyof State>(ALWAYS);
  const sig = plateSignature(new Proxy(s, { get: (t, k, r) => (read.add(k as keyof State), Reflect.get(t, k, r)) }));
  watched = [...read].filter((k) => !NEVER.has(k));
  return sig;
}

const advance = (ctx: SliceContext, ev: SliceEvent) => {
  const r = (ev as { result: SliceResult }).result;
  return { previous: ctx.result ?? ctx.previous, result: r, error: null };
};
const annotated = (ctx: SliceContext, e: Extract<SliceEvent, { type: 'ANNOTATE' }>) => {
  const fill = (r: SliceResult | null) => (r?.signature === e.signature ? { ...r, summary: { ...r.summary, ...e.part } } : r);
  const r = cache.get(e.signature);
  if (r) cache.set(e.signature, fill(r)!);
  return { result: fill(ctx.result), previous: fill(ctx.previous) };
};
const ANNOTATE = { action: annotated };
const changed = [
  { target: 'stale', guard: (ctx: SliceContext) => !!ctx.result },
  { target: 'editing' },
] satisfies { target: SliceStateName; guard?: (ctx: SliceContext) => boolean }[];
const common = {
  CHANGE: changed,
  CANCEL: changed,
  RESET: { target: 'editing', action: () => ({ result: null, previous: null, target: null, pct: 0, stage: '', error: null }) },
  START: { target: 'slicing', action: (_: SliceContext, e: { signature: string }) => ({ target: e.signature, pct: 0, stage: 'Preparing', error: null }) },
  RESTORE: { target: 'sliced', action: advance },
  ANNOTATE,
} as const;

export const slice = createMachine<SliceStateName, SliceEvent, SliceContext>({
  id: 'slice', initial: 'editing',
  context: { result: null, previous: null, target: null, pct: 0, stage: '', error: null },
  states: {
    editing: { note: 'not sliced', on: common },
    stale: { note: 'Out of date—showing your previous slice until you slice again', on: common },
    slicing: {
      on: { ...common,
        PROGRESS: { action: (_, e) => ({ pct: e.pct, stage: e.stage }) },
        ERROR: { target: 'failed', action: (_, e) => ({ error: e.error }) },
      },
    },
    sliced: { on: common },
    failed: { on: common },
  },
});

let wired = false;
export function startSliceTracking() {
  if (wired) return;
  wired = true;
  let generation = documentGeneration();
  seen = getState();
  let lastSignature = signature(seen);
  subscribeHistory(() => {
    if (generation === documentGeneration()) return;
    generation = documentGeneration();
    lastSignature = signature();
    slice.send({ type: 'RESET' });
  });
  const active = (j: JobView) => (j.state === 'submitting' || j.state === 'running') && !j.calib && !!j.plateSignature;
  subscribeStore(() => {
    const s = getState(), was = seen;
    seen = s;
    if (s.job !== was.job && active(s.job)) {
      if (!active(was.job) || was.job.plateSignature !== s.job.plateSignature) slice.send({ type: 'START', signature: s.job.plateSignature! });
      else slice.send({ type: 'PROGRESS', pct: s.job.pct, stage: s.job.stage });
    } else if (s.job !== was.job && active(was.job) && slice.get().value === 'slicing') {
      if (s.job.state === 'failed') slice.send({ type: 'ERROR', error: s.job.error ?? 'Not sliced' });
      else if (s.job.state !== 'done') slice.send({ type: 'CANCEL' });
    }
    if (s.job !== was.job && s.job.state === 'done' && !s.job.calib && s.job.plateSignature &&
        s.job.id !== slice.get().context.result?.job.id && (s.job.id !== was.job.id || was.job.state !== 'done')) {
      const r: SliceResult = { signature: s.job.plateSignature, job: s.job, summary: summarize(s.job), at: Date.now() };
      keep(r);
      if (!s.job.stale && r.signature === signature(s)) slice.send({ type: 'RESTORE', result: r });
    }
    if (watched.some((k) => s[k] !== was[k])) {
      const sig = signature(s);
      if (sig !== lastSignature) {
        lastSignature = sig; slice.send({ type: 'CHANGE' });
        if (s.job.state === 'failed' && !s.job.id) setState({ job: IDLE_JOB });
      }
    }
  });
  slice.subscribe((snap) => { if (snap.context.result && snap.value === 'sliced') keep(snap.context.result); });
}

export function cancelSlice() {
  slice.send({ type: 'CANCEL' });
  const result = slice.get().context.result;
  setState({ job: result ? { ...result.job, stale: true } : { state: 'canceled', pct: 0, stage: '' } });
}

export type SliceSnapshot = Snapshot<SliceStateName, SliceContext>;
export function useSlice(): SliceSnapshot;
export function useSlice<T>(sel: (s: SliceSnapshot) => T): T;
export function useSlice<T>(sel?: (s: SliceSnapshot) => T) {
  return useMachine(slice, sel);
}

export const currentResult = (): SliceResult | null => slice.get().context.result;
export const isStale = (s: SliceSnapshot = slice.get()) => !!s.context.result && s.value !== 'sliced';
export const isFresh = (s: SliceSnapshot = slice.get()) => s.value === 'sliced';

const minus = (a: number | null, b: number | null) => (a == null || b == null ? null : a - b);
let memo: { a: SliceResult; b: SliceResult; d: SliceDiff } | null = null;
const without = (a: EngineWarning[], b: EngineWarning[]) => { const keys = new Set(b.map(warningKey)); return a.filter((w) => !keys.has(warningKey(w))); };

export function sliceDiff(ctx: SliceContext = slice.get().context): SliceDiff | null {
  const { result: a, previous: b } = ctx;
  if (!a || !b) return null;
  if (memo?.a === a && memo.b === b) return memo.d;
  const x = a.summary, y = b.summary;
  const d: SliceDiff = {
    timeS: minus(x.timeS, y.timeS), grams: minus(x.grams, y.grams), supportG: minus(x.supportG, y.supportG), layers: minus(x.layers, y.layers),
    supportGone: !!y.supportG && x.supportG === 0, supportAdded: y.supportG === 0 && !!x.supportG,
    warningsAdded: without(x.warnings, y.warnings), warningsCleared: without(y.warnings, x.warnings),
  };
  memo = { a, b, d };
  return d;
}

let waiting: (() => void) | null = null;
export function whenFresh(f: () => void) {
  waiting?.(); waiting = null;
  if (isFresh()) return f();
  if (slice.get().value !== 'slicing') return;
  const off = slice.subscribe((s) => {
    if (s.value === 'slicing') return;
    off(); waiting = null;
    if (isFresh(s)) f();
  });
  waiting = off;
}

export const annotate = (signature: string, part: Partial<Pick<SliceSummary, 'supportG' | 'layers'>>) => slice.send({ type: 'ANNOTATE', signature, part });

(window as unknown as { __slice: unknown }).__slice = { machine: slice, cache, sliceDiff, signature };
