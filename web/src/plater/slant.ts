import type * as THREE from 'three';
import { T3 } from '../three-runtime';
import { effectiveConfig, getState, setState, updateObject, type Instance, type PlateObject, type SlantEdit, type State } from '../state/store';
import type { Config } from '../api/types';
import { instanceBox } from './geometry';
import { loadHead, type Head } from '../heads';
import { OPTIONS } from '../options';

const DEG = Math.PI / 180;
const KEYS = ['nonplanar_slant_angle', 'nonplanar_slant_direction', 'nonplanar_slant_start_height'] as const;
export const MAX_TILT = 45;

export function slantTarget(s: State = getState()): { o: PlateObject; inst: Instance } | null {
  const sel = s.selected;
  const o = sel && !sel.all ? s.objects.find((ob) => ob.id === sel.object) : undefined;
  const inst = o?.instances.find((i) => i.id === sel!.instance);
  return o && inst && !o.project?.placeholder ? { o, inst } : null;
}

export function slantNormal(e: Pick<SlantEdit, 'tilt' | 'dir'>): THREE.Vector3 {
  const t = e.tilt * DEG, d = e.dir * DEG;
  return new T3.Vector3(Math.sin(t) * Math.cos(d), Math.sin(t) * Math.sin(d), Math.cos(t));
}

export function slantAngles(n: THREE.Vector3): { tilt: number; dir: number } {
  const v = n.clone().normalize();
  if (v.z < 0) v.negate();
  const tilt = Math.acos(Math.min(1, v.z)) / DEG;
  const dir = tilt < 1e-6 ? 0 : ((Math.atan2(v.y, v.x) / DEG) % 360 + 360) % 360;
  return { tilt, dir };
}

function span(b: THREE.Box3, dirDeg: number): number {
  const d = dirDeg * DEG, u = [Math.cos(d), Math.sin(d)];
  let lo = Infinity, hi = -Infinity;
  for (const x of [b.min.x, b.max.x]) for (const y of [b.min.y, b.max.y]) {
    const p = x * u[0] + y * u[1];
    lo = Math.min(lo, p);
    hi = Math.max(hi, p);
  }
  return hi - lo;
}

const morphing = (s: State, o: PlateObject) => num(objectValue(s, o, 'nonplanar_slant_morph_layers'), 0) > 0;

export function slantPoint(s: State, e: SlantEdit): THREE.Vector3 | null {
  const t = slantTarget(s);
  if (!t) return null;
  const b = instanceBox(t.o, t.inst);
  const rise = morphing(s, t.o) ? 0 : Math.tan(e.tilt * DEG) * span(b, e.dir) / 2;
  return new T3.Vector3((b.min.x + b.max.x) / 2, (b.min.y + b.max.y) / 2, b.min.z + e.z + rise);
}

function lowestStart(s: State, e: SlantEdit): number {
  const t = slantTarget(s);
  if (!t || morphing(s, t.o)) return 0;
  return -Math.floor(Math.tan(e.tilt * DEG) * span(instanceBox(t.o, t.inst), e.dir) * 100) / 100;
}

const num = (v: unknown, fallback: number) => (v === undefined || v === '' || !Number.isFinite(Number(v)) ? fallback : Number(v));
const objectValue = (s: State, o: PlateObject, key: string) => o.settings[key] ?? effectiveConfig(s, 'process')[key];

export function initialSlant(s: State = getState()): SlantEdit | null {
  const t = slantTarget(s);
  if (!t) return null;
  const own = t.o.settings.nonplanar_slant_angle, angle = own === undefined ? 0 : num(own, 0);
  const dir = num(objectValue(s, t.o, 'nonplanar_slant_direction'), -1);
  const start = num(objectValue(s, t.o, 'nonplanar_slant_start_height'), 0);
  return { z: start, tilt: angle > 0 ? Math.min(angle, MAX_TILT) : 10, dir: dir >= 0 ? dir : 90, dirAuto: dir < 0, auto: angle <= 0 && dir < 0 && start <= 0 };
}

export function openSlantPlane() {
  const s = getState();
  if (s.gizmo === 'slant') return;
  const e = initialSlant(s);
  if (!e) return;
  setState({ gizmo: 'slant', slant: e });
  void refreshSlantLimit();
}

export const closeSlantPlane = () => setState({ gizmo: 'none', slant: null });

export function setSlant(patch: Partial<SlantEdit>) {
  const p: Partial<SlantEdit> = { ...patch };
  for (const k of ['z', 'tilt', 'dir'] as const) if (p[k] !== undefined && !Number.isFinite(p[k])) delete p[k];
  if (p.tilt !== undefined) p.tilt = Math.max(0, Math.min(MAX_TILT, p.tilt));
  if (p.dir !== undefined) { p.dir = ((p.dir % 360) + 360) % 360; p.dirAuto ??= false; }
  setState((s) => {
    if (!s.slant) return {};
    const e = { ...s.slant, ...p, auto: false };
    return { slant: { ...e, z: Math.max(lowestStart(s, e), e.z) } };
  });
  void refreshSlantLimit();
}

export function commitSlant() {
  const s = getState(), e = s.slant, t = slantTarget(s);
  if (!e || !t) return closeSlantPlane();
  const settings: Config = { ...t.o.settings };
  if (e.auto) for (const k of KEYS) delete settings[k];
  else {
    settings.nonplanar_slant_angle = String(Math.round(e.tilt * 100) / 100);
    settings.nonplanar_slant_direction = e.dirAuto ? '-1' : String(Math.round((((e.dir % 360) + 360) % 360) * 100) / 100);
    settings.nonplanar_slant_start_height = String(Math.round(e.z * 100) / 100);
  }
  settings.nonplanar_mode = 'slant';
  updateObject(t.o.id, { settings }, 'Slant plane');
  closeSlantPlane();
}

export function resetSlantAuto() {
  setState((s) => (s.slant ? { slant: { z: 0, tilt: 10, dir: 90, dirAuto: true, auto: true } } : {}));
  void refreshSlantLimit();
}

export function slantLimit(head: Head, dirDeg: number, width: number, h: number, ignoreDuct: boolean, tol = 0): number {
  const isDuct = (b: Head['boxes'][number]) => {
    const x = b as { duct?: boolean; block?: boolean; name?: string };
    return x.duct ?? (!b.nozzle && !x.block && /duct|shroud/i.test(x.name ?? ''));
  };
  const boxes = ignoreDuct ? head.boxes.filter((b) => !isDuct(b)) : head.boxes;
  const rise = dirDeg * DEG + Math.PI, u = [Math.cos(rise), Math.sin(rise)];
  const slope = (gap: number, nozzle: boolean) => {
    let tan = Infinity;
    for (const box of boxes) {
      if (box.infinite_x || box.infinite_y || (!nozzle && box.nozzle)) continue;
      const grow = box.nozzle ? 0 : head.lateral_margin;
      const lo = [box.min[0] - grow, box.min[1] - grow], hi = [box.max[0] + grow, box.max[1] + grow];
      let pmin = Infinity, pmax = -Infinity;
      for (const c of [lo, hi, [lo[0], hi[1]], [hi[0], lo[1]]]) {
        const d = c[0] * u[0] + c[1] * u[1];
        pmin = Math.min(pmin, d);
        pmax = Math.max(pmax, d);
      }
      if (pmax <= 0 || pmin > width) continue;
      const n = gap + box.min[2] - (box.nozzle ? 0 : head.margin);
      tan = Math.min(tan, n >= 0 ? n / Math.min(pmax, width) : pmin > 0 ? n / pmin : -Infinity);
    }
    return tan === Infinity ? Math.PI / 2 : Math.atan(Math.max(0, tan));
  };
  tol = Math.max(0, tol);
  let limit = Math.min(slope(h + tol, true), slope(tol, false));
  const gantry = Math.min(...boxes.filter((b) => b.infinite_x || b.infinite_y).map((b) => b.min[2]));
  if (Number.isFinite(gantry)) limit = Math.min(limit, Math.atan(Math.max(0, gantry + tol - head.margin) / width));
  return Math.max(0, limit / DEG - 3);
}

export async function refreshSlantLimit() {
  const s = getState(), e = s.slant, t = slantTarget(s);
  if (!e || !t) return;
  const machine = effectiveConfig(s, 'machine'), process = effectiveConfig(s, 'process');
  const head = await loadHead(String(machine.nonplanar_head ?? ''));
  const now = getState().slant;
  if (!now) return;
  if (!head) return setState({ slant: { ...now, limit: undefined } });
  const width = span(instanceBox(t.o, t.inst), now.dir);
  const duct = ['1', 'true'].includes(String(process.nonplanar_ignore_duct ?? '0'));
  const tolerance = num(process.nonplanar_collision_tolerance ?? OPTIONS.nonplanar_collision_tolerance?.default, 0);
  const limit = slantLimit(head, now.dir, Math.max(width, 1e-3), num(objectValue(s, t.o, 'layer_height'), 0.2), duct, tolerance);
  setState({ slant: { ...now, limit } });
}
