import { objectValues } from '../profiles/view';
import * as THREE from 'three';
import { arrangeAll, bedArea } from '../actions';
import type { Config } from '../api/types';
import { instanceBox, objectMatrix, outsideBed } from '../plater/geometry';
import { scaleToBuildVolume } from '../plater/objectOps';
import { largestPlane, layOnFace, layOnFacePlanes, linearOf } from '../plater/transforms';
import { undoable } from '../state/history';
import { getState, setState, subscribeStore, type PlateObject, type State } from '../state/store';
import { selectionValues } from '../profiles/view';
import type { Doc } from '../state/history';
import type { PreflightIn, PreflightOut } from './preflight.worker';
import { threeReady } from '../three-runtime';

export type IssueKind = 'bed' | 'overhang' | 'contact' | 'footprint';
export interface Issue {
  key: string;
  objectId: number;
  name: string;
  kind: IssueKind;
  tris: Uint32Array;
  fix: { label: string; run(): void;  edit?: (s: State) => Partial<Doc> };
}

const REACH_MM = 6, REGION_MM2 = 10, POINT_CONTACT_MM2 = 5, SMALL_FOOT_MM2 = 50, TALL_MM = 10;

const on = (v: unknown) => v === true || v === 1 || v === '1' || v === 'true';
const num = (v: unknown, d: number) => { const n = parseFloat(String(Array.isArray(v) ? v[0] : v)); return Number.isFinite(n) ? n : d; };

let worker: Worker | null = null;
let seq = 0;
const waiting = new Map<number, { resolve(o: PreflightOut): void; reject(e: Error): void; key: string }>();
const cache = new Map<string, Promise<PreflightOut>>();
function analysis(o: PlateObject, firstLayer: number, threshold: number, nozzle: number): Promise<PreflightOut> {
  const m = objectMatrix(o).elements;
  const key = `${o.geometry.uuid}:${m.map((e) => e.toFixed(4)).join()}:${firstLayer}:${threshold}:${nozzle}`;
  let p = cache.get(key);
  if (p) return p;
  const fail = (e: Event) => {
    const error = new Error(e instanceof ErrorEvent && e.message ? e.message : 'Oops! The background model checker stopped');
    worker?.terminate(); worker = null;
    for (const w of waiting.values()) { cache.delete(w.key); w.reject(error); }
    waiting.clear();
  };
  worker ??= Object.assign(new Worker(new URL('./preflight.worker.ts', import.meta.url), { type: 'module' }), {
    onmessage: (e: MessageEvent<PreflightOut>) => { waiting.get(e.data.id)?.resolve(e.data); waiting.delete(e.data.id); },
    onerror: fail, onmessageerror: fail,
  });
  const pos = o.geometry.attributes.position.array as Float32Array;
  const index = o.geometry.index ? Uint32Array.from(o.geometry.index.array) : Uint32Array.from({ length: pos.length / 3 }, (_, i) => i);
  const id = ++seq;
  const msg: PreflightIn = { id, positions: Float32Array.from(pos), index, matrix: [...m], firstLayer, threshold, nozzle };
  p = new Promise<PreflightOut>((resolve, reject) => waiting.set(id, { resolve, reject, key }));
  worker.postMessage(msg, [msg.positions.buffer, msg.index.buffer]);
  if (cache.size > 64) cache.delete(cache.keys().next().value!);
  cache.set(key, p);
  return p;
}

const withSettings = (s: State, id: number, patch: Config) => ({ objects: s.objects.map((o) => (o.id === id ? { ...o, settings: { ...o.settings, ...patch } } : o)) });
const settingsFix = (label: string, id: number, patch: Config): Issue['fix'] => ({
  label, edit: (s) => withSettings(s, id, patch), run: () => undoable(label, () => setState((s) => withSettings(s, id, patch))),
});
function layFlat(o: PlateObject): Issue['fix'] | null {
  const hull = (o.geometry.userData.hull as Float32Array | undefined) ?? (o.geometry.attributes.position.count <= 50_000 ? o.geometry.attributes.position.array : null);
  const inst = o.instances[0];
  if (!hull || !inst) return null;
  const planes = layOnFacePlanes(hull, linearOf(o, inst)), best = largestPlane(planes);
  if (best < 0) return null;
  const next = (s: State) => ({ objects: s.objects.map((x) => (x.id === o.id ? layOnFace(x, inst.id, planes[best].normal) : x)) });
  return { label: 'Lay flat', edit: next, run: () => undoable('Lay flat', () => setState(next)) };
}

let issues: Issue[] = [];
const dismissed = new Set<string>();
const listeners = new Set<() => void>();
let version = 0;
const emit = () => { version++; listeners.forEach((l) => l()); };

async function check(s: State) {
  const mine = ++run;
  await threeReady;
  if (mine !== run) return;
  const printer = selectionValues(s, 'machine'), print = selectionValues(s, 'process'), nozzle = s.records.printers.find((p) => p.id === s.printer)?.nozzles[0]?.diameter;
  if (printer.state !== 'ready' || print.state !== 'ready' || !printer.status?.ok || !print.status?.ok || !nozzle) { issues = []; emit(); return; }
  const machine = printer.values;
  const area = bedArea(), height = num(machine.printable_height, 0);
  const out = area.length ? outsideBed(s.objects, area, height) : new Set<number>();
  const next: Issue[] = [];
  for (const o of s.objects) {
    if (o.project?.placeholder || !o.instances.length) continue;
    const cfg = objectValues(s, o.id).values;
    const push = (kind: IssueKind, tris: Uint32Array, fix: Issue['fix'] | null) => fix && next.push({ key: `${o.id}:${kind}`, objectId: o.id, name: o.name, kind, tris, fix });
    if (o.instances.some((i) => out.has(i.id))) {
      const b = instanceBox(o, o.instances[0]), size = b.getSize(new THREE.Vector3());
      const xs = area.map((p) => p.x), ys = area.map((p) => p.y);
      const fits = size.x <= Math.max(...xs) - Math.min(...xs) && size.y <= Math.max(...ys) - Math.min(...ys) && size.z <= height;
      push('bed', new Uint32Array(), fits ? { label: 'Arrange', run: () => arrangeAll(false) } : { label: 'Scale to fit', run: () => scaleToBuildVolume(o.id) });
    }
    const a = await analysis(o, num(cfg.initial_layer_print_height, 0.2), num(cfg.support_threshold_angle, 30) || 30, nozzle).catch(() => null);
    if (mine !== run) return;
    if (!a) continue;
    const point = a.height > 2 && a.contact < POINT_CONTACT_MM2;
    if (point) push('contact', new Uint32Array(), layFlat(o));
    else if (a.height > TALL_MM && a.contact < SMALL_FOOT_MM2 && String(cfg.brim_type) === 'no_brim')
      push('footprint', new Uint32Array(), settingsFix('Add brim', o.id, { brim_type: 'outer_only', brim_width: '5' }));
    if (!point && !on(cfg.enable_support)) {
      const falling = a.overhangs.filter((r) => !r.bridge && r.reach >= REACH_MM && r.area >= REGION_MM2);
      if (falling.length) {
        const tris = new Uint32Array(falling.reduce((n, r) => n + r.tris.length, 0));
        falling.reduce((at, r) => (tris.set(r.tris, at), at + r.tris.length), 0);
        push('overhang', tris, settingsFix('Add support', o.id, { enable_support: '1' }));
      }
    }
  }
  issues = next;
  emit();
}

let run = 0, timer = 0, last: unknown[] = [];
subscribeStore(() => {
  const s = getState(), now = [s.objects, s.resolved, s.records.printers, s.projectPlates, s.plateIndex];
  if (now.every((v, i) => v === last[i])) return;
  last = now;
  clearTimeout(timer);
  timer = window.setTimeout(() => void check(getState()), 300);
});
void check(getState());

export const plateIssues = () => issues.filter((i) => !dismissed.has(i.key) && getState().objects.some((o) => o.id === i.objectId));
export function dismissIssue(key: string) { dismissed.add(key); emit(); }
export const subscribeIssues = (l: () => void) => { listeners.add(l); return () => { listeners.delete(l); }; };
export const issuesVersion = () => version;

let hovered: string | null = null;
export const hoveredIssue = () => hovered;
export function hoverIssue(key: string | null) { if (hovered !== key) { hovered = key; emit(); } }
