import type * as THREE from 'three';
import { T3 } from './three-runtime';
import { bedArea } from './actions';
import { bedBounds, instanceBox, nearestFreeSpot } from './plater/geometry';
import { undoable } from './state/history';
import { getState, isSelected, markStale, newId, removeSelected, setState, toast, updateObject, type Instance, type PlateObject, type State } from './state/store';

export function selectionTargets(s: State = getState()): { o: PlateObject; inst: Instance }[] {
  const sel = s.selected;
  if (!sel) return [];
  return s.objects.filter((o) => !o.project?.placeholder)
    .flatMap((o) => o.instances.filter((i) => isSelected(sel, o.id, i.id)).map((inst) => ({ o, inst })));
}

function selectionBox(s: State = getState()): THREE.Box3 {
  const box = new T3.Box3();
  for (const { o, inst } of selectionTargets(s)) box.union(instanceBox(o, inst));
  return box;
}

function shiftSelection(name: string, dx: number, dy: number, dz: number) {
  const ids = new Set(selectionTargets().map((t) => t.inst.id));
  if (!ids.size) return;
  undoable(name, () => setState((s) => ({
    objects: s.objects.map((o) => o.instances.some((i) => ids.has(i.id))
      ? { ...o, instances: o.instances.map((i) => ids.has(i.id) ? { ...i, x: i.x + dx, y: i.y + dy, z: (i.z ?? 0) + dz } : i) } : o),
    job: markStale(s.job),
  })));
}

export function selectAll() {
  const first = getState().objects[0];
  if (first) setState({ selected: { object: first.id, instance: first.instances[0].id, all: true } });
}

export function centreSelection() {
  const box = selectionBox();
  if (box.isEmpty()) return;
  const { center } = bedBounds(bedArea());
  shiftSelection('Center', center.x - (box.min.x + box.max.x) / 2, center.y - (box.min.y + box.max.y) / 2, 0);
}

export function canDrop(s: State = getState()): boolean {
  const box = selectionBox(s);
  return !box.isEmpty() && Math.abs(box.min.z) > 1e-3;
}

export function dropSelection() {
  if (canDrop()) shiftSelection('Drop to bed', 0, 0, -selectionBox().min.z);
}

export async function mirrorSelection(axis: 0 | 1 | 2) {
  const { mirror } = await import('./plater/transforms');
  const targets = selectionTargets();
  if (!targets.length) return;
  const c = selectionBox().getCenter(new T3.Vector3());
  const several = targets.length > 1;
  const ids = new Set(targets.map((t) => t.inst.id));
  undoable('Mirror', () => setState((s) => ({
    objects: s.objects.map((o) => {
      const first = targets.find((t) => t.o.id === o.id);
      if (!first) return o;
      const m = mirror(o, first.inst.id, axis);
      if (!several || axis === 2) return m;
      return { ...m, instances: m.instances.map((i) => !ids.has(i.id) ? i : axis === 0 ? { ...i, x: 2 * c.x - i.x } : { ...i, y: 2 * c.y - i.y }) };
    }),
    job: markStale(s.job),
  })));
}

export const canPaste = (s: State = getState()) => s.clipboard.length > 0;

export function copySelection() {
  const targets = selectionTargets();
  if (!targets.length) return;
  if (targets.some((t) => t.o.project)) return toast('objects_inside_locked_3mf');
  const byObject = new Map<PlateObject, Instance[]>();
  for (const { o, inst } of targets) byObject.set(o, [...(byObject.get(o) ?? []), inst]);
  setState({ clipboard: [...byObject].map(([o, instances]) => ({ ...o, settings: { ...o.settings }, scale: [...o.scale], instances: instances.map((i) => ({ ...i })) })) });
}

export function cutSelection() {
  const before = getState().clipboard;
  copySelection();
  if (getState().clipboard !== before) removeSelected();
}

export function pasteClipboard() {
  const s = getState();
  if (!canPaste(s)) {
    if (s.clipboard.length) toast('cant_paste_into_locked');
    return;
  }
  const box = new T3.Box3();
  for (const o of s.clipboard) for (const i of o.instances) box.union(instanceBox(o, i));
  const size = box.getSize(new T3.Vector3());
  const from = new T3.Vector2((box.min.x + box.max.x) / 2, (box.min.y + box.max.y) / 2);
  const spot = nearestFreeSpot(s.objects, new T3.Vector2(size.x, size.y), from, bedArea());
  const dx = spot ? spot.x - from.x : size.x + 6, dy = spot ? spot.y - from.y : 0;
  const pasted = s.clipboard.map((o) => ({
    ...o, id: newId(), settings: { ...o.settings },
    instances: o.instances.map((i) => ({ ...i, id: newId(), x: i.x + dx, y: i.y + dy })),
  }));
  const last = pasted[pasted.length - 1];
  undoable('Paste', () => setState((st) => ({
    objects: [...st.objects, ...pasted],
    selected: { object: last.id, instance: last.instances[0].id, allInstances: true },
    job: markStale(st.job),
  })));
}

export function startRename(id = getState().selected?.object) {
  const o = getState().objects.find((ob) => ob.id === id);
  if (!o) return;
  if (o.project) return toast('part_names_are_locked');
  setState({ renaming: o.id, contextMenu: null });
}

export function renameObject(id: number, name: string) {
  if (getState().renaming !== id) return;
  const trimmed = name.trim();
  setState({ renaming: null });
  if (trimmed && trimmed !== getState().objects.find((o) => o.id === id)?.name) updateObject(id, { name: trimmed }, 'Rename');
}

export function setFilament(id: number, extruder: number) {
  const s = getState();
  const saved = s.objects.find((o) => o.id === id)?.project?.saved?.object;
  const hit = (o: PlateObject) => o.id === id || saved !== undefined && o.project?.saved?.object === saved;
  const set = (o: PlateObject) => (hit(o) ? { ...o, extruder } : o);
  undoable('Change Filament', () => setState((st) => ({
    objects: st.objects.map(set),
    projectPlates: st.projectPlates.map((p) => (p.index === st.plateIndex ? p : { ...p, objects: p.objects.map(set) })),
    job: markStale(st.job),
  })));
}
