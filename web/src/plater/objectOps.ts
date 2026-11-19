import * as THREE from 'three';
import { fileAccept } from '../file-accept';
import { bedArea } from '../actions';
import { getState, newId, printerPreset, setState, toast, updateVolume, type PlateObject, type Volume } from '../state/store';
import { undoable } from '../state/history';
import { centred, instanceBox, instanceMatrix, loadMesh, request, volumeBox, volumeWorld } from './geometry';
import objectKeys from '../object_options.json';

const replace = (name: string, objects: PlateObject[], selected?: { object: number; instance: number }) =>
  undoable(name, () => setState({ objects, ...(selected ? { selected } : {}) }));

export function scaleToBuildVolume(id: number) {
  const s = getState(), o = s.objects.find((x) => x.id === id), machine = printerPreset(s);
  if (!o || !machine) return;
  const area = bedArea(), xs = area.map((p) => p.x), ys = area.map((p) => p.y);
  const box = instanceBox(o, o.instances[0]), size = box.getSize(new THREE.Vector3());
  const height = Number(machine.config.printable_height ?? 0) || Infinity;
  const f = 0.99 * Math.min((Math.max(...xs) - Math.min(...xs)) / size.x, (Math.max(...ys) - Math.min(...ys)) / size.y, height / size.z);
  if (!Number.isFinite(f) || f <= 0) return;
  const cx = (Math.min(...xs) + Math.max(...xs)) / 2, cy = (Math.min(...ys) + Math.max(...ys)) / 2;
  replace('Scale To Fit', s.objects.map((x) => x.id !== id ? x : { ...x, scale: x.scale.map((v) => v * f) as [number, number, number],
    instances: x.instances.map((i, k) => (k ? i : { ...i, x: cx, y: cy, z: 0 })) }));
}

const UNIT = { inch: 25.4, meter: 1000 } as const;

export function convertUnits(id: number, unit: 'inch' | 'meter' | null) {
  const s = getState(), o = s.objects.find((x) => x.id === id);
  if (!o) return;
  const f = unit ? UNIT[unit] / (o.units ? UNIT[o.units] : 1) : 1 / (o.units ? UNIT[o.units] : 1);
  const label = unit ? `Convert from ${unit}` : `Restore to ${o.units}`;
  replace(label, s.objects.map((x) => x.id !== id ? x : { ...x, scale: x.scale.map((v) => v * f) as [number, number, number], units: unit ?? undefined,
    instances: x.instances.map((i) => ({ ...i, z: 0 })) }));
}


export function mergeObjects(ids: number[]) {
  const s = getState(), objects = s.objects.filter((o) => ids.includes(o.id) && !o.project);
  if (objects.length < 2) return;
  const [first, ...rest] = objects, i0 = first.instances[0];
  const into = instanceMatrix(first, i0).invert();
  const volumes: Volume[] = [...(first.volumes ?? [])];
  for (const o of rest) {
    const m = into.clone().multiply(instanceMatrix(o, o.instances[0]));
    const [geometry, back] = centred(o.geometry);
    volumes.push({ id: newId(), name: o.name, type: 'part', geometry, matrix: m.clone().multiply(back).elements, extruder: o.extruder === first.extruder ? 0 : o.extruder,
      settings: { ...o.settings }, paint: o.paint });
    for (const v of o.volumes ?? []) volumes.push({ ...v, id: newId(), matrix: m.clone().multiply(new THREE.Matrix4().fromArray(v.matrix)).elements });
  }
  const merged: PlateObject = { ...first, volumes, instances: [i0], layerProfile: undefined };
  const gone = new Set(rest.map((o) => o.id));
  replace('Merge', s.objects.filter((o) => !gone.has(o.id)).map((o) => (o.id === first.id ? merged : o)), { object: merged.id, instance: i0.id });
}

export async function partsToObjects(id: number) {
  const s = getState(), o = s.objects.find((x) => x.id === id);
  const parts = o?.volumes?.filter((v) => v.type === 'part') ?? [];
  if (!o || !parts.length) return;
  const inst = o.instances[0];
  const objects: PlateObject[] = [];
  for (const v of parts) {
    const g = v.geometry.clone().applyMatrix4(volumeWorld(o, inst, v));
    const at = new THREE.Box3().setFromBufferAttribute(g.attributes.position as THREE.BufferAttribute);
    const file = new File([stl(g)], `${v.name}.stl`, { type: 'model/stl' });
    objects.push({ ...o, id: newId(), name: v.name, geometry: await loadMesh(file), source: file, rotX: 0, rotY: 0, scale: [1, 1, 1], volumes: undefined,
      extruder: v.extruder || o.extruder, settings: { ...o.settings, ...v.settings }, paint: undefined, layerProfile: undefined, units: undefined,
      instances: [{ id: newId(), x: (at.min.x + at.max.x) / 2, y: (at.min.y + at.max.y) / 2, rotZ: 0, z: 0 }] });
  }
  const rest: PlateObject = { ...o, volumes: o.volumes!.filter((v) => v.type !== 'part') };
  replace('Set as individual objects', s.objects.flatMap((x) => (x.id === id ? [rest, ...objects] : [x])));
}

function stl(g: THREE.BufferGeometry): Blob {
  const p = g.attributes.position.array as Float32Array, idx = g.index?.array;
  const n = idx ? idx.length / 3 : p.length / 9;
  const buf = new ArrayBuffer(84 + 50 * n), dv = new DataView(buf);
  dv.setUint32(80, n, true);
  for (let t = 0; t < n; t++)
    for (let k = 0; k < 3; k++) {
      const v = idx ? idx[3 * t + k] : 3 * t + k;
      for (let a = 0; a < 3; a++) dv.setFloat32(84 + 50 * t + 12 + 12 * k + 4 * a, p[3 * v + a], true);
    }
  return new Blob([buf]);
}

export function replaceFile(id: number) {
  const input = document.createElement('input');
  input.type = 'file';
  input.accept = fileAccept(['stl', 'obj']);
  input.onchange = async () => {
    const file = input.files?.[0];
    if (!file) return;
    try {
      const geometry = await loadMesh(file);
      const s = getState();
      replace('Replace with 3D file', s.objects.map((x) => (x.id === id ? { ...x, geometry, source: file, paint: undefined, layerProfile: undefined } : x)));
    } catch (e) {
      toast('objectops_failed', { detail: e instanceof Error ? e.message : String(e) });
    }
  };
  input.click();
}

export function mirrorVolume(objectId: number, volumeId: number, axis: 0 | 1 | 2) {
  const o = getState().objects.find((x) => x.id === objectId), v = o?.volumes?.find((x) => x.id === volumeId);
  if (!o || !v) return;
  const inst = o.instances[0], c = volumeBox(o, inst, v).getCenter(new THREE.Vector3());
  const flip = new THREE.Matrix4().makeScale(axis === 0 ? -1 : 1, axis === 1 ? -1 : 1, axis === 2 ? -1 : 1);
  const world = new THREE.Matrix4().makeTranslation(c.x, c.y, c.z).multiply(flip).multiply(new THREE.Matrix4().makeTranslation(-c.x, -c.y, -c.z)).multiply(volumeWorld(o, inst, v));
  updateVolume(o.id, v.id, { matrix: instanceMatrix(o, inst).invert().multiply(world).elements }, 'Mirror');
}

export async function exportVolumeStl(objectId: number, volumeId: number) {
  const o = getState().objects.find((x) => x.id === objectId), v = o?.volumes?.find((x) => x.id === volumeId);
  if (!o || !v) return;
  try {
    const blob = await new Promise<Blob>((resolve, reject) => request({ type: 'bake', positions: v.geometry.attributes.position.array as Float32Array,
      index: (v.geometry.index?.array as Uint32Array | Uint16Array | undefined) ?? null, matrix: volumeWorld(o, o.instances[0], v).elements }, (m) => {
      if (m.type === 'blob') resolve(m.blob);
      else if (m.type === 'error') reject(new Error(m.message));
    }));
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = `${v.name.replace(/\.(stl|obj)$/i, '') || 'part'}.stl`;
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 60000);
  } catch (e) {
    toast('export_failed', { detail: e instanceof Error ? e.message : e });
  }
}

export function copySettings(id: number, volumeId?: number) {
  const o = getState().objects.find((x) => x.id === id);
  const v = volumeId === undefined ? undefined : o?.volumes?.find((x) => x.id === volumeId);
  if (!o) return;
  setState({ settingsClipboard: v ? { kind: 'volume', config: { ...o.settings, ...v.settings } } : { kind: 'object', config: { ...o.settings } } });
}

export function pasteSettings(targets: { object: number; volume?: number }[]) {
  const clip = getState().settingsClipboard;
  if (!clip || !targets.length) return;
  const { extruder: _e, ...config } = clip.config;
  const region = new Set(objectKeys.region);
  const forVolume = Object.fromEntries(Object.entries(config).filter(([k]) => region.has(k)));
  replace('Paste settings', getState().objects.map((o) => {
    const mine = targets.filter((t) => t.object === o.id);
    if (!mine.length) return o;
    if (clip.kind === 'object') return mine.some((t) => t.volume === undefined) ? { ...o, settings: { ...config } } : o;
    const ids = new Set(mine.map((t) => t.volume));
    return { ...o, volumes: o.volumes?.map((v) => (ids.has(v.id) ? { ...v, settings: { ...forVolume } } : v)) };
  }));
}
