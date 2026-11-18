import { createMachine } from '../flow/fsm';
import { getRecord, setRecord, sameRecord, type AnyRecord, type LibraryRecords } from './records';
import type { State } from '../state/store';
import { randomId } from '../random-id';

export type EntityKey = string;
export interface LibraryChange { base: Map<EntityKey, AnyRecord | undefined>; next: Map<EntityKey, AnyRecord | undefined> }
export interface LibraryWriteIntent {
  label: string;
  make(doc: LibraryRecords): LibraryChange;
  publish?(s: State, doc: LibraryRecords): Partial<State>;
  optimistic?: boolean;
  settle: { resolve(doc: LibraryRecords): void; reject(error: Error): void };
  forced?: Set<EntityKey>;
  change?: LibraryChange;
}
interface Frozen { operationId: string; body: string; expectedRevision: number; document: LibraryRecords }
interface Batch { intents: LibraryWriteIntent[]; frozen?: Frozen; rebases: number }
export interface LibraryTransport {
  read(): Promise<LibraryRecords>;
  encode(operationId: string, expectedRevision: number, document: LibraryRecords): string;
  put(body: string): Promise<LibraryRecords>;
  adopt(doc: LibraryRecords, published: LibraryWriteIntent[], written: Set<EntityKey>, pending: LibraryWriteIntent[]): void;
  loading(): boolean;
}

type WriteState = 'idle' | 'held' | 'writing' | 'saved' | 'offline' | 'failed' | 'conflicted';
type WriteEvent =
  | { type: 'SUBMIT'; intent: LibraryWriteIntent }
  | { type: 'ACK'; request: number; doc: LibraryRecords; written: Set<EntityKey> }
  | { type: 'REBASE'; request: number }
  | { type: 'CONFLICT'; request: number; keys: EntityKey[]; latest: LibraryRecords }
  | { type: 'OFFLINE'; request: number; error: string }
  | { type: 'FAIL'; request: number; error: string; retryable: boolean;  plain?: boolean }
  | { type: 'RETRY' } | { type: 'REAPPLY' } | { type: 'DISCARD' } | { type: 'DISMISS' }
  | { type: 'LIBRARY_READY' };
export interface WriteContext {
  request: number;
  inflight: Batch | null;
  queued: LibraryWriteIntent[];
  error: string | null;
  plain?: boolean;
  conflicts: EntityKey[];
  attempt: number;
}

const MAX_REBASES = 8;
let transport: LibraryTransport | undefined;
let known: LibraryRecords | undefined;
export const knownLibrary = () => known;
export function configureLibraryWrites(t: LibraryTransport) { transport = t; }
export function knowLibrary(doc: LibraryRecords) { if (!known || doc.revision >= known.revision || doc.epoch !== known.epoch) known = doc; }

export class LibraryWriteError extends Error {
  constructor(message: string, readonly code: string) { super(message); }
}

const ordered = (v: unknown) => JSON.stringify(v, (_, x) => x && typeof x === 'object' && !Array.isArray(x) ? Object.fromEntries(Object.entries(x).sort(([p], [q]) => p.localeCompare(q))) : x);
const sameField = (a: unknown, b: unknown) => ordered(a) === ordered(b);
const SETTING_MAPS = new Set(['settings', 'values', 'machine', 'printOverrides']);
type Fields = Record<string, unknown>;
function mergeRecord(base: AnyRecord | undefined, latest: AnyRecord | undefined, next: AnyRecord | undefined, force = false): AnyRecord | undefined | 'conflict' {
  if (sameRecord(latest, base)) return next;
  if (!base || !latest || !next) return force ? next : 'conflict';
  const b0 = base as unknown as Fields, l0 = latest as unknown as Fields, n0 = next as unknown as Fields;
  const out: Fields = { ...l0 };
  const take = (b: unknown, l: unknown, n: unknown) => sameField(b, l) || sameField(l, n) || force;
  for (const field of new Set([...Object.keys(b0), ...Object.keys(l0), ...Object.keys(n0)])) {
    if (field === 'revision' || field === 'edited') continue;
    const b = b0[field], l = l0[field], n = n0[field];
    if (sameField(b, n)) continue;
    if (SETTING_MAPS.has(field)) {
      const bm = (b ?? {}) as Fields, lm = (l ?? {}) as Fields, nm = (n ?? {}) as Fields, merged: Fields = { ...lm };
      for (const key of new Set([...Object.keys(bm), ...Object.keys(nm)])) {
        if (sameField(bm[key], nm[key])) continue;
        if (!take(bm[key], lm[key], nm[key])) return 'conflict';
        if (nm[key] === undefined) delete merged[key]; else merged[key] = nm[key];
      }
      if (Object.keys(merged).length) out[field] = merged; else delete out[field];
      continue;
    }
    if (!take(b, l, n)) return 'conflict';
    if (n === undefined) delete out[field]; else out[field] = n;
  }
  return out as unknown as AnyRecord;
}
function freeze(batch: Batch, latest: LibraryRecords): { document: LibraryRecords; conflicts: EntityKey[]; written: Set<EntityKey> } {
  const start = new Map<EntityKey, AnyRecord | undefined>(), conflicts: EntityKey[] = [], written = new Set<EntityKey>();
  let document = latest;
  for (const intent of batch.intents) {
    const change = intent.change = intent.make(document);
    for (const [key, next] of change.next) {
      const current = getRecord(document, key);
      if (!start.has(key)) start.set(key, current);
      if (sameRecord(current, next)) continue;
      const own = !sameRecord(current, start.get(key)) && sameRecord(start.get(key), change.base.get(key));
      const merged = mergeRecord(change.base.get(key), current, next, !!intent.forced?.has(key) || own);
      if (merged === 'conflict') { conflicts.push(key); continue; }
      const archived = (r: AnyRecord | undefined) => !!(r as { archived?: unknown } | undefined)?.archived;
      if (archived(current) && archived(merged) && !archived(change.base.get(key)))
        throw new LibraryWriteError(`Oops! ${(current as { name?: string }).name ?? 'This profile'} was removed in another window, so we couldn't save this change — but don't worry, your edits are safe.`, 'removed');
      document = setRecord(document, key, merged);
      written.add(key);
    }
  }
  return { document, conflicts: [...new Set(conflicts)], written };
}

const unreachable = (error: unknown) => error instanceof TypeError || [0, 502, 504].includes((error as { status?: number }).status ?? -1);
const message = (error: unknown) => error instanceof Error ? error.message : String(error);
const settleAll = (intents: LibraryWriteIntent[], how: (i: LibraryWriteIntent) => void) => intents.forEach(how);

async function run(batch: Batch, request: number) {
  await Promise.resolve();
  const mine = () => libraryWrites.get().context.request === request;
  try {
    if (!batch.frozen) {
      const latest = known ?? await transport!.read();
      knowLibrary(latest);
      const { document, conflicts, written } = freeze(batch, latest);
      if (conflicts.length) { if (mine()) libraryWrites.send({ type: 'CONFLICT', request, keys: conflicts, latest }); return; }
      if (!written.size) { if (mine()) libraryWrites.send({ type: 'ACK', request, doc: latest, written }); return; }
      const operationId = `library:${randomId()}`;
      batch.frozen = { operationId, expectedRevision: latest.revision, document, body: transport!.encode(operationId, latest.revision, document) };
      writtenBy.set(batch, written);
    }
    const doc = await transport!.put(batch.frozen.body);
    knowLibrary(doc);
    if (mine()) libraryWrites.send({ type: 'ACK', request, doc, written: writtenBy.get(batch) ?? new Set() });
  } catch (error) {
    if (!mine()) return;
    const code = (error as { code?: string }).code, status = (error as { status?: number }).status ?? 0;
    if (code === 'revision_conflict') {
      try { known = await transport!.read(); } catch (read) { libraryWrites.send({ type: 'OFFLINE', request, error: message(read) }); return; }
      batch.frozen = undefined;
      if (++batch.rebases > MAX_REBASES) libraryWrites.send({ type: 'FAIL', request, error: 'Whoa, your library is busy changing somewhere else! Give it a moment and try again.', retryable: true });
      else libraryWrites.send({ type: 'REBASE', request });
    } else if (unreachable(error)) libraryWrites.send({ type: 'OFFLINE', request, error: message(error) });
    else {
      libraryWrites.send({ type: 'FAIL', request, error: message(error), retryable: status >= 500, plain: code === 'name_taken' });
      if (code === 'removed') libraryWrites.send({ type: 'DISMISS' });
    }
  }
}
const writtenBy = new WeakMap<Batch, Set<EntityKey>>();

const queue = { action: (c: WriteContext, e: { intent: LibraryWriteIntent }) => {
  const queued = [...c.queued, e.intent];
  if (e.intent.optimistic && known) transport!.adopt(known, [], new Set(), [...c.inflight?.intents ?? [], ...queued]);
  return { queued };
} };
const mine = (c: WriteContext, e: { request: number }) => e.request === c.request;
const submit = [{ target: 'held' as const, guard: () => !!transport?.loading(), action: queue.action }, { target: 'writing' as const, action: queue.action }];
const late = {};
const startNext = (c: WriteContext): Partial<WriteContext> => ({ inflight: c.inflight ?? { intents: c.queued, rebases: 0 }, queued: c.inflight ? c.queued : [] });

export const libraryWrites = createMachine<WriteState, WriteEvent, WriteContext>({
  id: 'library-writes', initial: 'idle', strict: true,
  context: { request: 0, inflight: null, queued: [], error: null, conflicts: [], attempt: 0 },
  states: {
    idle: { on: { SUBMIT: submit, ACK: late, REBASE: late, CONFLICT: late, OFFLINE: late, FAIL: late, RETRY: {}, DISMISS: {}, LIBRARY_READY: {} } },
    held: {
      note: 'the library is loading; the intents wait and go out once it is loaded',
      on: { SUBMIT: queue, LIBRARY_READY: { target: 'writing' }, DISCARD: { target: 'idle', action: (c) => discard(c) }, ACK: late, REBASE: late, CONFLICT: late, OFFLINE: late, FAIL: late, RETRY: {}, DISMISS: {} },
    },
    writing: {
      note: 'one PUT in flight; new intents queue behind it',
      entry: (c) => { const next = startNext(c), request = c.request + 1; void run(next.inflight!, request); return { ...next, request, error: null, conflicts: [] }; },
      on: {
        SUBMIT: queue,
        ACK: [{ target: 'writing', guard: (c, e) => mine(c, e) && c.queued.length > 0, action: (c, e) => acknowledged(c, e) },
          { target: 'saved', guard: mine, action: (c, e) => acknowledged(c, e) }, late],
        REBASE: [{ target: 'writing', guard: mine }, late],
        CONFLICT: [{ target: 'conflicted', guard: mine, action: (c, e) => { conflicted(c, e.latest); return { conflicts: e.keys }; } }, late],
        OFFLINE: [{ target: 'offline', guard: mine, action: (c, e) => ({ error: e.error, attempt: c.attempt + 1 }) }, late],
        FAIL: [{ target: 'failed', guard: (c, e) => mine(c, e) && e.retryable, action: (_, e) => ({ error: e.error, plain: e.plain }) },
          { target: 'failed', guard: mine, action: (c, e) => { rejectBatch(c, new LibraryWriteError(e.error, e.plain ? 'name_taken' : 'rejected')); if (known) transport!.adopt(known, [], new Set(), c.queued); return { error: e.error, plain: e.plain, inflight: null }; } }, late],
        RETRY: {}, DISMISS: {}, LIBRARY_READY: {},
      },
    },
    saved: { after: { ms: 1500, target: 'idle' }, on: { SUBMIT: submit, ACK: late, REBASE: late, CONFLICT: late, OFFLINE: late, FAIL: late, RETRY: {}, DISMISS: { target: 'idle' }, LIBRARY_READY: {} } },
    offline: {
      note: 'the service is unreachable; the intents wait and are retried',
      after: { ms: (c) => Math.min(30_000, 1000 * 2 ** Math.min(c.attempt, 5)), target: 'writing' },
      on: { SUBMIT: queue, RETRY: { target: 'writing' }, DISCARD: { target: 'idle', action: (c) => discard(c) }, ACK: late, REBASE: late, CONFLICT: late, OFFLINE: late, FAIL: late, DISMISS: {}, LIBRARY_READY: {} },
    },
    failed: {
      note: 'inflight kept when retryable (Retry replays it), dropped and rejected otherwise',
      on: { SUBMIT: [{ target: 'writing', guard: (c) => !c.inflight, action: queue.action }, queue], RETRY: [{ target: 'writing', guard: (c) => !!c.inflight || c.queued.length > 0 }, { target: 'idle' }],
        DISCARD: { target: 'idle', action: (c) => discard(c) }, DISMISS: [{ target: 'writing', guard: (c) => !c.inflight && c.queued.length > 0 }, { target: 'idle', guard: (c) => !c.inflight, action: () => ({ error: null }) }],
        ACK: late, REBASE: late, CONFLICT: late, OFFLINE: late, FAIL: late, LIBRARY_READY: {} },
    },
    conflicted: {
      note: 'Reapply writes this tab\'s values over the latest; Discard keeps the latest',
      on: { SUBMIT: queue, REAPPLY: { target: 'writing', action: (c) => { for (const i of c.inflight!.intents) i.forced = new Set([...i.forced ?? [], ...c.conflicts]); c.inflight!.frozen = undefined; return {}; } },
        DISCARD: [{ target: 'writing', guard: (c) => c.queued.length > 0, action: (c) => discard(c) }, { target: 'idle', action: (c) => discard(c) }],
        ACK: late, REBASE: late, CONFLICT: late, OFFLINE: late, FAIL: late, RETRY: {}, DISMISS: {}, LIBRARY_READY: {} },
    },
  },
});

function acknowledged(c: WriteContext, e: { doc: LibraryRecords; written: Set<EntityKey> }): Partial<WriteContext> {
  const intents = c.inflight!.intents;
  for (const intent of c.queued) for (const [key, base] of intent.change?.base ?? []) {
    const own = intents.find((i) => i.change?.next.has(key) && sameRecord(i.change.base.get(key), base));
    if (own) intent.change!.base.set(key, own.change!.next.get(key));
  }
  transport!.adopt(e.doc, intents, e.written, c.queued);
  settleAll(intents, (i) => i.settle.resolve(e.doc));
  return { inflight: null, attempt: 0, error: null };
}
function conflicted(c: WriteContext, latest: LibraryRecords) { transport!.adopt(latest, [], new Set(), [...c.inflight?.intents ?? [], ...c.queued]); }
function rejectBatch(c: WriteContext, error: Error) { settleAll(c.inflight?.intents ?? [], (i) => i.settle.reject(error)); }
function discard(c: WriteContext): Partial<WriteContext> {
  rejectBatch(c, new LibraryWriteError(c.conflicts.length ? 'Kept the version saved elsewhere; your change was discarded.' : 'Your unsaved library change was discarded.', 'discarded'));
  if (known) transport!.adopt(known, [], new Set(), c.queued);
  return { inflight: null, error: null, conflicts: [], attempt: 0 };
}

export function submitLibraryWrite(intent: Pick<LibraryWriteIntent, 'label' | 'publish' | 'optimistic'> & ({ change: LibraryChange } | { make: LibraryWriteIntent['make'] })): Promise<LibraryRecords> {
  const make = 'make' in intent ? intent.make : () => intent.change, change = 'change' in intent ? intent.change : undefined;
  return new Promise((resolve, reject) => libraryWrites.send({ type: 'SUBMIT', intent: { label: intent.label, publish: intent.publish, optimistic: intent.optimistic, make, change, settle: { resolve, reject } } }));
}

export function libraryWritesPending(): boolean {
  const { value, context } = libraryWrites.get();
  return !!context.inflight || context.queued.length > 0 || value === 'writing';
}
export function libraryWritesIdle(): Promise<void> {
  return new Promise((resolve) => {
    const check = () => { if (libraryWrites.get().value !== 'writing') { stop?.(); resolve(); return true; } return false; };
    let stop: (() => void) | undefined;
    if (!check()) stop = libraryWrites.subscribe(() => void check());
  });
}
export function flushLibraryWrites(): Promise<void> {
  return new Promise((resolve, reject) => {
    let stop: (() => void) | undefined;
    const check = () => {
      const { value, context } = libraryWrites.get();
      if (value === 'writing' || value === 'held') return;
      stop?.();
      if (value === 'offline') reject(new Error('Your library changes aren\'t saved yet — we can\'t reach the service right now.'));
      else if (value === 'conflicted') reject(new Error('Oops, a library change clashed with one saved somewhere else! Pick Reapply or Discard first.'));
      else if (value === 'failed' && context.inflight) reject(new Error(`Uh-oh, we couldn't save your library changes: ${context.error}`));
      else resolve();
    };
    check();
    if (libraryWrites.get().value === 'writing') stop = libraryWrites.subscribe(check);
  });
}

if (typeof window !== 'undefined') {
  window.addEventListener('online', () => libraryWrites.send({ type: 'RETRY' }));
  window.addEventListener('beforeunload', (e) => { if (libraryWritesPending()) { e.preventDefault(); e.returnValue = ''; } });
}
