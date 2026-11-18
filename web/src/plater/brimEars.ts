import { objectValues } from '../profiles/view';
import * as THREE from 'three';
import { effectiveConfig, getState, markStale, setState, type PlateObject, type State } from '../state/store';
import { undoable } from '../state/history';
import { instanceMatrix } from './geometry';

export type Ear = NonNullable<PlateObject['brimEars']>[number];

export function defaultEarRadius(s: State = getState()): number {
  const nozzle = s.records.printers.find((p) => p.id === s.printer)?.nozzles[0]?.diameter ?? 0;
  const w = String(effectiveConfig(s, 'process').initial_layer_line_width ?? '0');
  const width = w.endsWith('%') ? (parseFloat(w) / 100) * nozzle : parseFloat(w) || nozzle;
  return Math.min(100, Math.max(0.1, width * 8));
}

export const setEars = (id: number, ears: Ear[], name: string) =>
  undoable(name, () => setState((s) => ({ objects: s.objects.map((o) => (o.id === id ? { ...o, brimEars: ears } : o)), job: markStale(s.job) })));

type P = [number, number];

export function sliceLoops(g: THREE.BufferGeometry, world: THREE.Matrix4, z: number): P[][] {
  const pos = g.attributes.position, idx = g.index, n = idx ? idx.count : pos.count;
  const v = [new THREE.Vector3(), new THREE.Vector3(), new THREE.Vector3()];
  const key = (p: P) => `${Math.round(p[0] * 1e4)},${Math.round(p[1] * 1e4)}`;
  const next = new Map<string, P[]>(), starts: P[][] = [];
  for (let t = 0; t < n; t += 3) {
    for (let k = 0; k < 3; k++) v[k].fromBufferAttribute(pos, idx ? idx.getX(t + k) : t + k).applyMatrix4(world);
    const cut: P[] = [];
    for (let k = 0; k < 3; k++) {
      const a = v[k], b = v[(k + 1) % 3];
      if ((a.z - z) * (b.z - z) < 0) { const u = (z - a.z) / (b.z - a.z); cut.push([a.x + u * (b.x - a.x), a.y + u * (b.y - a.y)]); }
    }
    if (cut.length !== 2) continue;
    const normal = new THREE.Vector3().subVectors(v[1], v[0]).cross(new THREE.Vector3().subVectors(v[2], v[0]));
    const dir: P = [-normal.y, normal.x];
    const [a, b] = (cut[1][0] - cut[0][0]) * dir[0] + (cut[1][1] - cut[0][1]) * dir[1] >= 0 ? cut : [cut[1], cut[0]];
    const list = next.get(key(a)) ?? [];
    list.push(b);
    next.set(key(a), list);
    starts.push([a, b]);
  }
  const used = new Set<string>(), loops: P[][] = [];
  for (const [a] of starts) {
    if (used.has(key(a))) continue;
    const loop: P[] = [];
    let p = a;
    while (!used.has(key(p))) {
      used.add(key(p));
      loop.push(p);
      const succ = next.get(key(p));
      if (!succ?.length) break;
      p = succ.shift()!;
    }
    if (loop.length > 2) loops.push(loop);
  }
  return loops;
}

const area = (l: P[]) => l.reduce((s, p, i) => { const q = l[(i + 1) % l.length]; return s + p[0] * q[1] - q[0] * p[1]; }, 0) / 2;

function douglasPeucker(pts: P[], tol: number): P[] {
  if (pts.length < 3) return pts;
  const [a, b] = [pts[0], pts[pts.length - 1]];
  let worst = -1, far = 0;
  for (let i = 1; i < pts.length - 1; i++) {
    const dx = b[0] - a[0], dy = b[1] - a[1], L = Math.hypot(dx, dy);
    const d = L ? Math.abs(dy * pts[i][0] - dx * pts[i][1] + b[0] * a[1] - b[1] * a[0]) / L : Math.hypot(pts[i][0] - a[0], pts[i][1] - a[1]);
    if (d > far) [far, worst] = [d, i];
  }
  if (far <= tol) return [a, b];
  return [...douglasPeucker(pts.slice(0, worst + 1), tol).slice(0, -1), ...douglasPeucker(pts.slice(worst), tol)];
}

function cornerPoints(poly: P[], threshold: number, convex: boolean): P[] {
  const cos = Math.cos(threshold + 1e-12), out: P[] = [];
  let p1 = poly[poly.length - 1], v1: P = [p1[0] - poly[poly.length - 2][0], p1[1] - poly[poly.length - 2][1]];
  for (const p2 of poly) {
    const v2: P = [p2[0] - p1[0], p2[1] - p1[1]];
    const cross = v1[0] * v2[1] - v1[1] * v2[0];
    const dot = (v1[0] * v2[0] + v1[1] * v2[1]) / (Math.hypot(...v1) * Math.hypot(...v2) || 1);
    if ((convex ? cross > 0 : cross < 0) && (threshold <= 1e-9 || dot < cos)) out.push(p1);
    [v1, p1] = [v2, p2];
  }
  return out;
}

export function autoGenerate(o: PlateObject, maxAngle: number, detection: number, radius: number) {
  const inst = o.instances[0], world = instanceMatrix(o, inst), back = world.clone().invert();
  const threshold = ((180 - maxAngle) * Math.PI) / 180;
  const ears: Ear[] = [...(o.brimEars ?? [])];
  for (const loop of sliceLoops(o.geometry, world, 0.1)) {
    const outer = area(loop) > 0;
    let pts = loop;
    if (detection > 0) {
      pts = douglasPeucker([...loop, loop[0]], detection);
      if (pts.length > 4) pts = pts.slice(0, -1);
    }
    if (pts.length < 3) continue;
    for (const [x, y] of cornerPoints(outer ? pts : [...pts].reverse(), threshold, outer)) {
      const q = new THREE.Vector3(x, y, -0.0001).applyMatrix4(back);
      if (!ears.some((e) => Math.hypot(e.p[0] - q.x, e.p[1] - q.y, e.p[2] - q.z) < 1e-6)) ears.push({ p: [q.x, q.y, q.z], r: radius });
    }
  }
  setEars(o.id, ears, 'Auto generate brim ear');
}

export const brimPainted = (s: State, o: PlateObject) => String(objectValues(s, o.id).values.brim_type ?? '') === 'painted';

export interface EarsUi { radius: number; maxAngle: number; detection: number; selected: number[] }
let ui: EarsUi = { radius: 0, maxAngle: 125, detection: 1, selected: [] };
const listeners = new Set<() => void>();
export const earsUi = () => ui;
export const subscribeEarsUi = (l: () => void) => (listeners.add(l), () => void listeners.delete(l));
export function setEarsUi(patch: Partial<EarsUi>) {
  ui = { ...ui, ...patch };
  listeners.forEach((l) => l());
}
