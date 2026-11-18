import { createMachine, type StateNode } from '../flow/fsm';
import { projectId } from './identity';
import { jsonWrite, serviceJson } from './service';
import type { NoticeOwner } from '../api/access';
export type JsonValue = null | boolean | number | string | readonly JsonValue[] | { readonly [key: string]: JsonValue };
export type ServiceDocument = Record<string, unknown>;
export type DocumentSyncState = 'loading' | 'ready' | 'debounced' | 'writing' | 'failed' | 'conflict';
export type DocumentSnapshot<D> = { state: DocumentSyncState; revision: number; document: D; error: string | null; code: string | null };
type SyncEvent = { type: 'PATCH' } | { type: 'HYDRATED' } | { type: 'WRITE'; retry: boolean; auto: boolean } | { type: 'ACK' }
  | { type: 'FAIL'; error: string; code: string | null; conflict: boolean; transient: boolean } | { type: 'ONLINE' } | { type: 'RELOAD' };
interface SyncContext { error: string | null; code: string | null; transient: boolean; retryDelay: number }
export type KeyMerge = (remote: unknown, local: unknown) => unknown;
const same = (a: unknown, b: unknown) => JSON.stringify(a) === JSON.stringify(b);
export function createServiceDocument<D extends ServiceDocument>(path: string, method: 'PUT' | 'PATCH', owner: NoticeOwner, beforeWrite: (document: D) => Promise<unknown> = () => Promise.resolve(), merge: Partial<Record<keyof D & string, KeyMerge>> = {}) {
  let current: DocumentSnapshot<D> = { state: 'loading', revision: 0, document: {} as D, error: null, code: null };
  let hydrated = false, loading: Promise<DocumentSnapshot<D>> | null = null, running: Promise<void> | null = null;
  let serial = 0, savedSerial = 0;
  let beforeHydration: Partial<D> = {};
  const unsaved = new Map<string, number>();
  let base = {} as D;
  let operation: { operationId: string; expectedRevision: number; document: D; serial: number } | null = null;
  const listeners = new Set<() => void>();
  const transient = (error: unknown) => { const status = (error as { status?: number }).status ?? 0;
    return error instanceof TypeError || error instanceof DOMException || status === 408 || status === 429 || status >= 500; };
  const auto = () => { void write(false, true).catch(() => {}); };
  const fail = [
    { target: 'conflict' as const, guard: (_: SyncContext, e: Extract<SyncEvent, { type: 'FAIL' }>) => e.conflict, action: (_: SyncContext, e: Extract<SyncEvent, { type: 'FAIL' }>) => ({ error: e.error, code: e.code, transient: false }) },
    { target: 'failed' as const, action: (c: SyncContext, e: Extract<SyncEvent, { type: 'FAIL' }>) =>
      ({ error: e.error, code: e.code, transient: e.transient, retryDelay: e.transient ? Math.min(c.retryDelay * 2 || 1000, 10_000) : c.retryDelay }) },
  ];
  const refail = { action: (_: SyncContext, e: Extract<SyncEvent, { type: 'FAIL' }>) => ({ error: e.error, code: e.code }) };
  const hydratedAfterFailure = [{ target: 'debounced' as const, guard: () => savedSerial < serial, action: () => ({ error: null, code: null }) }, { target: 'ready' as const, action: () => ({ error: null, code: null }) }];
  const edits: StateNode<DocumentSyncState, SyncEvent, SyncContext>['on'] = { PATCH: 'debounced', WRITE: 'writing', RELOAD: 'loading' };
  const sync = createMachine<DocumentSyncState, SyncEvent, SyncContext>({
    id: `service-document:${path}`, initial: 'loading', strict: true, context: { error: null, code: null, transient: false, retryDelay: 0 },
    states: {
      loading: { on: { ...edits, HYDRATED: 'ready', FAIL: fail } },
      ready: { on: edits },
      debounced: { on: { ...edits, HYDRATED: {}, FAIL: fail }, after: { ms: 500, action: () => { void flush().catch(() => {}); } } },
      writing: { on: { PATCH: {}, HYDRATED: {}, RELOAD: 'loading', FAIL: fail,
        ACK: [{ target: 'debounced', guard: () => savedSerial < serial, action: () => ({ error: null, code: null, retryDelay: 0 }) }, { target: 'ready', action: () => ({ error: null, code: null, retryDelay: 0 }) }] } },
      failed: { on: { PATCH: {}, RELOAD: 'loading', FAIL: refail, HYDRATED: hydratedAfterFailure,
        WRITE: { target: 'writing', guard: (c, e) => e.retry || (e.auto && c.transient) },
        ONLINE: { guard: (c) => c.transient, action: auto } },
        after: { ms: (c) => c.retryDelay, guard: (c) => c.transient, action: auto } },
      conflict: { on: { PATCH: {}, RELOAD: 'loading', FAIL: refail, WRITE: { target: 'writing', guard: (_, e) => e.retry } } },
    },
  });
  if (typeof window !== 'undefined') window.addEventListener('online', () => { if (sync.get().value === 'failed') sync.send({ type: 'ONLINE' }); });
  if (typeof window !== 'undefined') window.addEventListener('pagehide', () => { if (sync.get().value === 'debounced') void flush().catch(() => {}); });
  const publish = (patch: Partial<DocumentSnapshot<D>>) => { current = { ...current, ...patch }; listeners.forEach((listener) => listener()); };
  sync.subscribe(({ value, context }) => publish({ state: value, error: context.error, code: context.code }));
  const message = (error: unknown) => error instanceof Error ? error.message : String(error);
  const codeOf = (error: unknown) => (error as { code?: unknown })?.code as string | undefined ?? null;
  const hydrate = () => {
    if (hydrated) return Promise.resolve(current);
    loading ??= serviceJson<{revision:number;document:D}>(path).then((remote) => {
      if (!remote || !Number.isSafeInteger(remote.revision) || remote.revision < 0 || !remote.document || typeof remote.document !== 'object' || Array.isArray(remote.document)) throw new Error('Oops! The saved project file on the server couldn\'t be read.');
      hydrated = true; base = remote.document;
      publish({ ...remote, document: { ...remote.document, ...beforeHydration } });
      beforeHydration = {};
      sync.send({ type: 'HYDRATED' });
      return current;
    }).catch((error: unknown) => {
      loading = null; sync.send({ type: 'FAIL', error: message(error), code: codeOf(error), conflict: false, transient: transient(error) }); throw error;
    });
    return loading;
  };
  function patch(document: Partial<D>) {
    const captured = structuredClone(document) as Record<string, unknown>;
    for (const key of Object.keys(captured)) if (same(captured[key], (current.document as Record<string, unknown>)[key])) delete captured[key];
    if (!Object.keys(captured).length) return;
    if (!hydrated) beforeHydration = { ...beforeHydration, ...captured };
    serial++; for (const key of Object.keys(captured)) unsaved.set(key, serial);
    publish({ document: { ...current.document, ...captured } });
    sync.send({ type: 'PATCH' });
  }
  const flush = (retry = false) => write(retry, false);
  async function write(retry: boolean, automatic: boolean): Promise<void> {
    if (running) { await running; if (savedSerial < serial) return write(retry, automatic); return; }
    if (!sync.can({ type: 'WRITE', retry, auto: automatic })) throw new Error(current.error ?? 'Changes couldn\'t be saved to the server yet');
    sync.send({ type: 'WRITE', retry, auto: automatic });
    running = (async () => {
      if (!hydrated) await hydrate();
      while (savedSerial < serial) {
        operation ??= { operationId: projectId(), expectedRevision: current.revision, document: current.document, serial };
        const captured = operation, { serial: _serial, ...body } = captured;
        await beforeWrite(captured.document);
        let ack: { revision: number } | null;
        try { ack = await serviceJson<{revision:number}>(path, jsonWrite(method, body), false, owner); }
        catch (error) {
          if ((error as { code?: string }).code !== 'revision_conflict') throw error;
          const carried = error as { revision?: number; document?: D };
          const remote = carried.document && Number.isSafeInteger(carried.revision) ? { revision: carried.revision!, document: carried.document }
            : await serviceJson<{revision:number;document:D}>(path);
          if (!remote || !Number.isSafeInteger(remote.revision) || !remote.document || typeof remote.document !== 'object' || Array.isArray(remote.document)) throw error;
          const next: Record<string, unknown> = { ...remote.document };
          for (const key of [...unsaved.keys()]) {
            const theirs = remote.document[key], mine = current.document[key];
            if (same(theirs, base[key])) next[key] = mine;
            else if (same(theirs, mine)) unsaved.delete(key);
            else if (merge[key as keyof D & string]) next[key] = merge[key as keyof D & string]!(theirs, mine);
            else if (retry) next[key] = mine;
            else throw error;
          }
          base = remote.document; operation = null; publish({ revision: remote.revision, document: next as D });
          continue;
        }
        if (!ack || ack.revision !== captured.expectedRevision + 1) throw new Error('Oops! The server returned an unexpected version number.');
        for (const [key, at] of unsaved) if (at <= captured.serial) unsaved.delete(key);
        base = captured.document; savedSerial = captured.serial; operation = null; publish({ revision: ack.revision });
      }
    })().then(() => { running = null; sync.send({ type: 'ACK' }); }, (error: unknown) => {
      running = null;
      sync.send({ type: 'FAIL', error: message(error), code: codeOf(error), conflict: codeOf(error) === 'revision_conflict', transient: transient(error) });
      throw error;
    });
    return running;
  }
  return {
    snapshot: () => current,
    subscribe: (listener: () => void) => { listeners.add(listener); return () => { listeners.delete(listener); }; },
    hydrate, patch, flush, machine: sync,
    async reload() {
      if (running) await running.catch(() => {});
      const previous = current, previousSerial = serial, previousSaved = savedSerial;
      loading = null; hydrated = false; beforeHydration = {}; serial = savedSerial = 0; unsaved.clear();
      sync.send({ type: 'RELOAD' });
      try { const remote = await hydrate(); operation = null; return remote; }
      catch (error) { serial += previousSerial; savedSerial = previousSaved; hydrated = true; publish({ revision: previous.revision, document: { ...previous.document, ...beforeHydration } }); throw error; }
    },
  };
}
