import { useSyncExternalStore } from 'react';
import { createMachine } from './flow/fsm';
import type { DocumentSyncState, JsonValue } from './persist/service-document';
type Controller = typeof import('./persist/preferences');
type Status = { state: 'loading' | 'ready' | 'pending' | 'failed'; error: string | null; hydrated: boolean;  unreachable?: boolean };
let values: Record<string, JsonValue> = {}, queued: Record<string, JsonValue> = {};
let status: Status = { state: 'loading', error: null, hydrated: false }, controller: Controller | null = null;
const listeners = new Set<() => void>();
const keyListeners = new Map<string, Set<() => void>>();
const same = (a: unknown, b: unknown) => a === b || JSON.stringify(a) === JSON.stringify(b);
function publish(next: Record<string, JsonValue>, nextStatus: Status) {
  const previous = values; values = next; status = nextStatus;
  for (const key of new Set([...Object.keys(previous), ...Object.keys(next)]))
    if (!same(previous[key], next[key])) keyListeners.get(key)?.forEach(listener => listener());
  listeners.forEach(listener => listener());
}
export const preference = (key: string) => values[key];
export function preferenceText(key: string): string | undefined {
  const value = preference(key); return value == null ? undefined : typeof value === 'string' ? value : String(value);
}
export function preferenceJson<T>(key: string, fallback: T): T {
  const value = preference(key);
  if (value === undefined || value === null) return fallback;
  if (typeof value !== 'string') return value as T;
  try { return JSON.parse(value) as T; } catch { return fallback; }
}
export function subscribePreference(key: string, listener: () => void) {
  const group = keyListeners.get(key) ?? new Set(); group.add(listener); keyListeners.set(key, group);
  return () => { group.delete(listener); if (!group.size) keyListeners.delete(key); };
}
export function observePreference(key: string, listener: () => void) { listener(); return subscribePreference(key, listener); }
export const usePreference = (key: string) => useSyncExternalStore(listener => subscribePreference(key, listener), () => preference(key));
export const savedPreferencesSnapshot = () => status;
export const subscribeSavedPreferences = (listener: () => void) => { listeners.add(listener); return () => { listeners.delete(listener); }; };
type BootState = 'idle' | 'starting' | 'attached' | 'failed';
type BootEvent = { type: 'START' } | { type: 'HYDRATED' } | { type: 'FAIL'; error: string; transient: boolean } | { type: 'RETRY' } | { type: 'ONLINE' };
interface BootContext { error: string | null; transient: boolean; retryDelay: number }
let loading: Promise<void> = Promise.resolve();
const start = (retry: boolean) => () => { loading = attach(retry); };
const boot = createMachine<BootState, BootEvent, BootContext>({
  id: 'saved-preferences', initial: 'idle', strict: true, context: { error: null, transient: false, retryDelay: 0 },
  states: {
    idle: { on: { START: { target: 'starting', action: start(false) } } },
    starting: { on: { START: {}, RETRY: {}, HYDRATED: { target: 'attached', action: () => ({ error: null, retryDelay: 0 }) },
      FAIL: { target: 'failed', action: (c, e) => ({ error: e.error, transient: e.transient, retryDelay: e.transient ? Math.min(c.retryDelay * 2 || 1000, 10_000) : c.retryDelay }) } } },
    attached: { on: { START: { action: () => { loading = Promise.resolve(); } }, RETRY: { action: () => { loading = controller!.flushServicePreferences(true); } } } },
    failed: { on: { START: { target: 'starting', action: start(false) }, RETRY: { target: 'starting', action: start(true) }, ONLINE: { target: 'starting', guard: (c) => c.transient, action: start(false) } },
      after: { ms: (c) => c.retryDelay, guard: (c) => c.transient, target: 'starting', action: start(false) } },
  },
});
const state = (s: DocumentSyncState): Status['state'] => s === 'debounced' || s === 'writing' ? 'pending' : s === 'conflict' ? 'failed' : s;
function refresh(next = controller ? controller.servicePreferencesSnapshot().document : { ...values, ...queued }) {
  const b = boot.get(), doc = controller?.servicePreferencesSnapshot();
  publish(next, b.value === 'failed' ? { state: 'failed', error: b.context.error, hydrated: false, unreachable: b.context.transient }
    : { state: doc ? state(doc.state) : Object.keys(queued).length ? 'pending' : 'loading', error: doc?.error ?? null, hydrated: b.value === 'attached' });
}
boot.subscribe(() => refresh());
if (typeof window !== 'undefined') window.addEventListener('online', () => { if (boot.get().value === 'failed') boot.send({ type: 'ONLINE' }); });
export function savePreferences(patch: Record<string, JsonValue>) {
  const changed = Object.fromEntries(Object.entries(patch).filter(([key, value]) => !same(values[key], value)));
  if (!Object.keys(changed).length) return;
  if (controller) controller.setServicePreferences(changed);
  else { queued = { ...queued, ...structuredClone(changed) }; refresh(); }
}
async function attach(retry: boolean) {
  try {
    if (!controller) {
      const loaded = await import('./persist/preferences');
      const edits = queued; queued = {}; controller = loaded;
      if (Object.keys(edits).length) controller.setServicePreferences(edits);
      controller.subscribeServicePreferences(() => refresh()); refresh();
    }
    await controller.hydrateServicePreferences();
    boot.send({ type: 'HYDRATED' });
    if (retry) await controller.flushServicePreferences(true);
  } catch (error) {
    if (boot.get().value === 'starting') {
      const status_ = (error as { status?: number }).status ?? 0;
      boot.send({ type: 'FAIL', error: error instanceof Error ? error.message : String(error),
        transient: error instanceof TypeError || error instanceof DOMException || status_ === 408 || status_ === 429 || status_ >= 500 });
    }
    throw error;
  }
}
export function initialiseSavedPreferences(retry = false): Promise<void> {
  boot.send({ type: retry && boot.get().value !== 'idle' ? 'RETRY' : 'START' });
  return loading;
}
export async function flushSavedPreferences() { await initialiseSavedPreferences(); await controller!.flushServicePreferences(); }
export const savedPreferencesMachine = boot;
