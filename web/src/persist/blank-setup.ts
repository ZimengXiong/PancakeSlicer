import type { Config } from '../api/types';
import { getState, setState, subscribeStore, type State } from '../state/store';
import { patchServiceSession, serviceSessionSnapshot, sessionDocument, sessionSettled } from './service-session';
import { serviceJson } from './service';
import { onLiveEvent } from '../live-events';
import { movePlateSetup } from '../state/plate-setup';

export interface BlankSetup { dirty: State['dirty']; processVariant: State['processVariant']; filamentEdits: Record<string, Config>;
  bedType?: string; flush?: { matrix?: number[] | null; multipliers?: number[] | null };
  times?: Record<string, number> }

const setupOf = (s: State): BlankSetup => ({ dirty: s.dirty, processVariant: s.processVariant, filamentEdits: s.filamentEdits });
const content = (b: BlankSetup | null | undefined) => b ? { dirty: b.dirty, processVariant: b.processVariant, filamentEdits: b.filamentEdits } : null;
const same = (a: BlankSetup | null | undefined, b: BlankSetup | null | undefined) => JSON.stringify(content(a)) === JSON.stringify(content(b));

const sameTimes = (a: Record<string, number> = {}, b: Record<string, number> = {}) => Object.keys(a).length === Object.keys(b).length && Object.entries(a).every(([k, t]) => b[k] === t);
function settings(b: BlankSetup | null | undefined): Map<string, unknown> {
  const out = new Map<string, unknown>();
  if (!b) return out;
  for (const type of ['machine', 'process'] as const) for (const [k, v] of Object.entries(b.dirty?.[type] ?? {})) out.set(`${type}\0${k}`, v);
  for (const [id, edits] of Object.entries(b.filamentEdits ?? {})) for (const [k, v] of Object.entries(edits)) out.set(`filament\0${id}\0${k}`, v);
  if (b.processVariant) out.set('variant', b.processVariant);
  return out;
}
function setupFrom(values: Map<string, unknown>, times: Record<string, number>, filaments: string[]): BlankSetup {
  const out: BlankSetup = { dirty: { machine: {}, process: {} }, processVariant: null, filamentEdits: Object.fromEntries(filaments.map((id) => [id, {}])), times };
  for (const [path, v] of values) {
    const [kind, a, b] = path.split('\0');
    if (kind === 'variant') out.processVariant = v as State['processVariant'];
    else if (kind === 'filament') (out.filamentEdits[a] ??= {})[b] = v as Config[string];
    else out.dirty[kind as 'machine' | 'process'][a] = v as Config[string];
  }
  return out;
}
export function mergeSetups(remote: BlankSetup | null | undefined, local: BlankSetup | null | undefined): BlankSetup | null {
  if (!remote) return local ?? null;
  if (!local) return remote;
  const rv = settings(remote), lv = settings(local), rt = remote.times ?? {}, lt = local.times ?? {};
  const values = new Map<string, unknown>(), times: Record<string, number> = {};
  for (const path of new Set([...rv.keys(), ...lv.keys(), ...Object.keys(rt), ...Object.keys(lt)])) {
    const mine = (lt[path] ?? -1) > (rt[path] ?? -1);
    const v = mine ? lv.get(path) : rv.get(path), t = mine ? lt[path] : rt[path];
    if (t !== undefined) times[path] = t;
    if (v !== undefined) values.set(path, v);
  }
  return setupFrom(values, times, [...new Set([...Object.keys(remote.filamentEdits ?? {}), ...Object.keys(local.filamentEdits ?? {})])]);
}

let started = false, adopting = false;
let times: Record<string, number> = {};
let clock = 0;
const stamp = () => clock = Math.max(Date.now(), clock + 1);
let writing: Promise<void> = Promise.resolve();
const write = (value: BlankSetup | null) => { patchServiceSession({ blankSetup: value }); writing = sessionSettled(); };
export function watchBlankSetup(blank: () => boolean): void {
  if (started) return;
  started = true;
  let booted = false;
  const adopt = (stored: BlankSetup | null | undefined) => {
    const told = booted; booted = true;
    if (!blank() || !stored) return;
    const { plateSetup, ...moved } = movePlateSetup(stored);
    const remote: BlankSetup = { ...moved, times: stored.times };
    if (plateSetup.plate || plateSetup.flushing) {
      const printer = getState().printer;
      void import('../profiles/plate-setup-move').then((m) => m.adoptStoredPlateSetup(plateSetup, printer, 'The untitled plate'));
      void write(remote);
    }
    const local: BlankSetup = { ...setupOf(getState()), times }, merged = mergeSetups(remote, local)!;
    times = merged.times ?? {};
    for (const t of Object.values(times)) clock = Math.max(clock, t);
    if (!same(merged, local)) {
      adopting = true;
      const before = getState();
      try { setState({ dirty: merged.dirty, processVariant: merged.processVariant, filamentEdits: { ...before.filamentEdits, ...merged.filamentEdits } }); }
      finally { adopting = false; }
      const after = getState();
      if (told) void Promise.all([import('../profiles/shared-draft'), import('../flow/library-sync'), import('../profiles/format'), import('../options')])
        .then(([{ sharedChanges }, { librarySync }, { display }, { OPTIONS }]) => librarySync.send({ type: 'SETUP_CHANGED_ELSEWHERE',
          moves: sharedChanges(before, after, (k, from, to) => ({ label: OPTIONS[k]?.label || k, from: display(k, from), to: display(k, to) })) }));
    }
    if (!same(merged, remote) || !sameTimes(merged.times, remote.times)) void write(merged);
  };
  adopt(sessionDocument().blankSetup as BlankSetup | undefined);
  onLiveEvent('session', (data) => {
    const revision = Number((JSON.parse(data || '{}') as { revision?: unknown }).revision);
    void writing.then(() => {
      if (Number.isSafeInteger(revision) && revision <= serviceSessionSnapshot().revision) return adopt(sessionDocument().blankSetup as BlankSetup | undefined);
      return serviceJson<{ document: { blankSetup?: BlankSetup } }>('/api/session').then((r) => adopt(r?.document.blankSetup ?? null));
    }).catch(() => {});
  });
  let last = setupOf(getState());
  subscribeStore(() => {
    const now = setupOf(getState());
    if (now.dirty === last.dirty && now.processVariant === last.processVariant && now.filamentEdits === last.filamentEdits || same(now, last)) return;
    const was = settings(last), is = settings(now);
    last = now;
    if (adopting || !blank()) return;
    for (const path of new Set([...was.keys(), ...is.keys()])) if (JSON.stringify(was.get(path)) !== JSON.stringify(is.get(path))) times = { ...times, [path]: stamp() };
    void write({ ...now, times });
  });
}

export function forgetBlankSetup(): void {
  if (!sessionDocument().blankSetup) return;
  void write(null);
}
