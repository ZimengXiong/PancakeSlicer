import type * as THREE from 'three';
import { T3 } from '../three-runtime';
import { getState, setState, type CutPlane, type Instance, type PlateObject, type State } from '../state/store';
import { instanceBox } from './geometry';

const DEG = Math.PI / 180;

const cuttable = (o: PlateObject) => !o.volumes?.length && !o.project?.placeholder;

export function cutTarget(s: State = getState()): { o: PlateObject; inst: Instance } | null {
  const sel = s.selected;
  const o = sel && !sel.all ? s.objects.find((ob) => ob.id === sel.object) : undefined;
  const inst = o?.instances.find((i) => i.id === sel!.instance);
  return o && inst && cuttable(o) ? { o, inst } : null;
}

export function cutBlocked(s: State = getState()): string | null {
  const o = s.objects.find((ob) => ob.id === s.selected?.object);
  if (!o || s.selected?.all) return 'Select one object to cut';
  if (o.project?.placeholder) return 'Oops! We couldn\'t find the 3D mesh for this object.';
  if (o.volumes?.length) return 'Objects with multiple parts or modifiers can\'t be cut with the plane tool yet.';
  return null;
}

export function cutNormal(c: Pick<CutPlane, 'rotX' | 'rotY'>): THREE.Vector3 {
  return new T3.Vector3(0, 0, 1).applyEuler(new T3.Euler(c.rotX * DEG, c.rotY * DEG, 0, 'YXZ'));
}

export function cutPoint(s: State, c: CutPlane): THREE.Vector3 | null {
  const t = cutTarget(s);
  if (!t) return null;
  const b = instanceBox(t.o, t.inst);
  return new T3.Vector3((b.min.x + b.max.x) / 2, (b.min.y + b.max.y) / 2, c.z);
}

export function initialCut(s: State = getState()): CutPlane | null {
  const t = cutTarget(s);
  if (!t) return null;
  const b = instanceBox(t.o, t.inst);
  return { z: (b.min.z + b.max.z) / 2, rotX: 0, rotY: 0, keepUpper: true, keepLower: true, placeLower: false };
}

export function toggleCut() {
  const s = getState();
  if (s.gizmo === 'cut') return setState({ gizmo: 'none', cut: null });
  if (cutBlocked(s)) return;
  setState({ gizmo: 'cut', cut: initialCut(s) });
}

export const setCut = (patch: Partial<CutPlane>) => setState((s) => (s.cut ? { cut: { ...s.cut, ...patch } } : {}));

export function canPerformCut(s: State = getState()): boolean {
  const t = cutTarget(s);
  const c = s.cut;
  if (!t || !c || !(c.keepUpper || c.keepLower) || c.editing) return false;
  if ((c.mode === 'dovetail' || c.connectors?.length) && !(c.keepUpper && c.keepLower && !c.toParts)) return false;
  const b = instanceBox(t.o, t.inst);
  const n = cutNormal(c), p = cutPoint(s, c)!;
  let lo = Infinity, hi = -Infinity;
  for (let k = 0; k < 8; k++) {
    const d = new T3.Vector3(k & 1 ? b.max.x : b.min.x, k & 2 ? b.max.y : b.min.y, k & 4 ? b.max.z : b.min.z).sub(p).dot(n);
    lo = Math.min(lo, d);
    hi = Math.max(hi, d);
  }
  return lo < -1e-3 && hi > 1e-3;
}
