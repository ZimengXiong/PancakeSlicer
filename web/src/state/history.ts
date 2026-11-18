import { captureProjectProfiles } from '../profiles/project';
import type * as THREE from 'three';
import { plateSignature } from '../slice-input';
import { getState, setState, type PlateObject, type State } from './store';

const DOC_KEYS = [
  'objects', 'projectPlates', 'plateIndex', 'project', 'projectName', 'projectMaterials', 'projectPresets', 'projectSlotValues', 'projectSlots',
  'events', 'printer', 'process', 'dirty', 'filamentEdits', 'selected',
  'wipeTowerPos', 'processVariant', 'profileSnapshots', 'projectProfileContexts',
] as const satisfies readonly (keyof State)[];
export type Doc = Pick<State, (typeof DOC_KEYS)[number]>;

interface Step { name: string; before: Doc; after: Doc; merge?: string; at: number }

export const MAX_STEPS = 100;
export const MAX_RETAINED_BYTES = 512 * 2 ** 20;
const MERGE_MS = 1500;

let undoStack: Step[] = [];
let redoStack: Step[] = [];
let open: { name: string; before: Doc; merge?: string; depth: number } | null = null;
let restoring = false;
let saved: Doc | null = null;
let generation = 0;

export const snapshot = (s: State = getState()): Doc => ({...Object.fromEntries(DOC_KEYS.map((k) => [k, s[k]])),profileSnapshots:captureProjectProfiles(s)}) as Doc;
const sameDoc = (a: Doc, b: Doc) => DOC_KEYS.every((k) => k === 'selected' || a[k] === b[k]);
const ids = new WeakMap<object, number>();
let lastId = 0;
const idOf = (v: object) => ids.get(v) ?? (ids.set(v, ++lastId), lastId);
const objects = (list: PlateObject[]) => list.map((o) => ({ ...o, geometry: `#${idOf(o.geometry)}`,
  volumes: o.volumes?.map((v) => ({ ...v, geometry: `#${idOf(v.geometry)}` })),
}));
const BOOKKEEPING = new Set(['revision', 'edited']);
const record = (v: object) => Object.fromEntries(Object.keys(v).filter((k) => !BOOKKEEPING.has(k)).sort().map((k) => [k, (v as Record<string, unknown>)[k]]));
const contents = (key: (typeof DOC_KEYS)[number], value: unknown) => JSON.stringify(
  key === 'objects' ? objects(value as PlateObject[]) : key === 'projectPlates' ? (value as Doc['projectPlates']).map((p) => ({ ...p, objects: objects(p.objects) })) : value,
  (_, v: unknown) => v && typeof v === 'object' && (v instanceof Blob || v instanceof Map) ? `#${idOf(v)}`
    : key === 'profileSnapshots' && v && typeof v === 'object' && !Array.isArray(v) ? record(v) : v);
export const sameContents = (a: Doc, b: Doc) => DOC_KEYS.every((k) => k === 'selected' || a[k] === b[k] || contents(k, a[k]) === contents(k, b[k]));

const listeners = new Set<() => void>();
let version = 0;
const notify = () => { version++; listeners.forEach((l) => l()); };
export const subscribeHistory = (l: () => void) => { listeners.add(l); return () => { listeners.delete(l); }; };
export const historyVersion = () => version;

export function captureUndo(): () => boolean {
  const step=undoStack.at(-1),document=generation,revision=version;
  return () => {
    if(!step || open || generation!==document || version!==revision || undoStack.at(-1)!==step || !sameDoc(snapshot(),step.after))return false;
    undo();return true;
  };
}

export function undoable<T>(name: string, fn: () => T, merge?: string): T {
  const end = beginStep(name, merge);
  try {
    return fn();
  } finally {
    end();
  }
}

export function beginStep(name: string, merge?: string): () => void {
  if (open) {
    open.depth++;
  } else open = { name, before: snapshot(), merge, depth: 1 };
  let ended = false;
  return () => {
    if (ended || !open) return;
    ended = true;
    if (--open.depth > 0) return;
    const { name: n, before, merge: m } = open;
    open = null;
    commit(n, before, m);
  };
}

function commit(name: string, before: Doc, merge?: string) {
  const after = snapshot();
  if (restoring || sameDoc(before, after) || sameContents(before, after)) return;
  const top = undoStack[undoStack.length - 1];
  const now = Date.now();
  if (top && merge && top.merge === merge && top.name === name && !redoStack.length && now - top.at < MERGE_MS && sameDoc(top.after, before)) {
    top.after = after;
    top.at = now;
  } else {
    if (top && !sameDoc(top.after, before)) top.after = before;
    undoStack.push({ name, before, after, merge, at: now });
  }
  redoStack = [];
  trim();
  notify();
}

function geometries(doc: Doc, into: Set<THREE.BufferGeometry>) {
  const add = (objects: PlateObject[]) => objects.forEach((o) => { into.add(o.geometry); o.volumes?.forEach((v) => into.add(v.geometry)); });
  add(doc.objects);
  doc.projectPlates.forEach((p) => add(p.objects));
}

const geometryBytes = (g: THREE.BufferGeometry) =>
  Object.values(g.attributes).reduce((n, a) => n + (a as THREE.BufferAttribute).array.byteLength, g.index?.array.byteLength ?? 0);

export function retainedBytes(): number {
  const live = new Set<THREE.BufferGeometry>();
  geometries(snapshot(), live);
  const held = new Set<THREE.BufferGeometry>();
  for (const st of [...undoStack, ...redoStack]) {
    geometries(st.before, held);
    geometries(st.after, held);
  }
  let n = 0;
  for (const g of held) if (!live.has(g)) n += geometryBytes(g);
  return n;
}

function trim() {
  if (undoStack.length > MAX_STEPS) undoStack.splice(0, undoStack.length - MAX_STEPS);
  while (undoStack.length > 1 && retainedBytes() > MAX_RETAINED_BYTES) undoStack.shift();
}

function restore(doc: Doc) {
  restoring = true;
  try {
    setState(doc);
    void import('../profiles/slot-writes').then(async ({ adoptStrip }) => adoptStrip((await import('../profiles/slots')).documentStrip(doc)));
    const s = getState();
    if (s.job.plateSignature && s.job.stale && plateSignature(s) === s.job.plateSignature)
      setState({ job: { ...s.job, stale: false } });
    const sel = getState().selected;
    if (sel && !getState().objects.some((o) => o.id === sel.object && o.instances.some((i) => i.id === sel.instance)))
      setState({ selected: null });
    else if (sel?.volume !== undefined && !getState().objects.some((o) => o.id === sel.object && o.volumes?.some((v) => v.id === sel.volume)))
      setState({ selected: { ...sel, volume: undefined, allInstances: true } });
  } finally {
    restoring = false;
  }
}

export function undo() {
  if (open || !undoStack.length) return;
  const st = undoStack.pop()!;
  const now = snapshot();
  if (!sameDoc(st.after, now)) st.after = now;
  restore(st.before);
  redoStack.push(st);
  notify();
}

export function redo() {
  if (open || !redoStack.length) return;
  const st = redoStack.pop()!;
  restore(st.after);
  undoStack.push(st);
  notify();
}

export function undoSteps(n: number) { for (let i = 0; i < n; i++) undo(); }
export function redoSteps(n: number) { for (let i = 0; i < n; i++) redo(); }
export const undoNames = (max = 10) => undoStack.slice(-max).reverse().map((st) => st.name);
export const redoNames = (max = 10) => redoStack.slice(-max).reverse().map((st) => st.name);

export const undoName = () => undoStack[undoStack.length - 1]?.name ?? null;
export const redoName = () => redoStack[redoStack.length - 1]?.name ?? null;
export const historyDepth = () => ({ undo: undoStack.length, redo: redoStack.length });

const leaving = new Set<() => void>();
export const onLeaveDocument = (f: () => void) => { leaving.add(f); };
export const leaveDocument = () => leaving.forEach((f) => f());

export function newDocument(isSaved: boolean) {
  undoStack = [];
  redoStack = [];
  open = null;
  saved = isSaved ? snapshot() : null;
  generation++;
  notify();
}
export const documentGeneration = () => generation;

export function markSaved(doc: Doc = snapshot()) {
  saved = doc;
  notify();
}

export function unsaved(s: State = getState()): boolean {
  const hasObjects = s.objects.length > 0 || s.projectPlates.some((p) => p.objects.length > 0);
  return hasObjects && (!saved || !sameDoc(saved, snapshot(s)));
}
