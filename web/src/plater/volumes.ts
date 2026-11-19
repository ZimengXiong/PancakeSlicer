import * as THREE from 'three';
import { fileAccept } from '../file-accept';
import { bedArea } from '../actions';
import { addVolume, getState, newId, setState, toast, type SeamMode, type Volume, type VolumeType } from '../state/store';
import { undoable } from '../state/history';
import { bakeStl, bedBounds, instanceBox, instanceMatrix, loadMesh, nextFreeSpot, primitiveGeometry, volumeGeometry } from './geometry';

export const TYPE_LABEL: Record<VolumeType, string> = {
  part: 'Part', negative: 'Negative Part', modifier: 'Modifier', support_blocker: 'Support Blocker', support_enforcer: 'Support Enforcer', seam: 'Precise Seam',
};

export type Shape = 'Cube' | 'Cylinder' | 'Sphere' | 'Cone' | 'Disc' | 'Torus';
export const SHAPES: Shape[] = ['Cube', 'Cylinder', 'Sphere', 'Cone', 'Disc', 'Torus'];

export const VOLUME_MENU: [VolumeType, string, string][] = [
  ['part', 'Add Part', 'menu_add_part'],
  ['negative', 'Add Negative Part', 'menu_add_negative'],
  ['modifier', 'Add Modifier', 'menu_add_modifier'],
  ['support_blocker', 'Add Support Blocker', 'menu_support_blocker'],
  ['support_enforcer', 'Add Support Enforcer', 'menu_support_enforcer'],
  ['seam', 'Add Precise Seam', 'menu_precise_seam_center'],
];
export const SEAM_MODES: [SeamMode, string][] = [['center', 'Seam Center'], ['left', 'Seam Left'], ['right', 'Seam Right'],
  ['enforced', 'Seam Enforced'], ['blocked', 'Seam Blocked'], ['neutral', 'Seam Neutral']];
export const volumeIcon = (type: VolumeType, mode?: SeamMode) => (type === 'seam' ? `menu_precise_seam_${mode ?? 'center'}` : VOLUME_MENU.find(([t]) => t === type)![2]);

export function placeVolume(objectId: number, type: VolumeType, name: string, geometry: THREE.BufferGeometry, world: (box: THREE.Box3, size: THREE.Vector3) => THREE.Vector3, step: string, extra: Partial<Volume> = {}) {
  const s = getState();
  const o = s.objects.find((ob) => ob.id === objectId);
  if (!o) return;
  const inst = o.instances.find((i) => i.id === s.selected?.instance) ?? o.instances[0];
  const size = geometry.boundingBox!.getSize(new THREE.Vector3());
  const at = world(instanceBox(o, inst), size);
  const matrix = instanceMatrix(o, inst).invert().multiply(new THREE.Matrix4().makeTranslation(at.x, at.y, at.z)).elements;
  const volume: Volume = { id: newId(), name, type, geometry, matrix, extruder: 0, settings: {}, ...extra };
  addVolume(o.id, volume, step);
  return volume.id;
}

export function addPrimitive(objectId: number, type: VolumeType, shape: Shape) {
  const { min, max } = bedBounds(bedArea());
  const geometry = primitiveGeometry(shape, 0.1 * Math.max(max.x - min.x, max.y - min.y));
  placeVolume(objectId, type, `Generic-${shape}`, geometry,
    (b, size) => new THREE.Vector3(b.max.x + size.x / 2, b.min.y + size.y / 2, b.min.z + size.z / 2), 'Add primitive');
}

export function loadVolume(objectId: number, type: VolumeType) {
  const input = document.createElement('input');
  input.type = 'file';
  input.accept = fileAccept(['stl', 'obj']);
  input.onchange = async () => {
    const file = input.files?.[0];
    if (!file) return;
    try {
      const loaded = await loadMesh(file);
      const geometry = volumeGeometry(loaded.attributes.position.array as Float32Array, loaded.index?.array as Uint32Array,
        loaded.attributes.normal?.array as Float32Array | undefined);
      placeVolume(objectId, type, file.name.replace(/\.(stl|obj)$/i, ''), geometry,
        (b, size) => new THREE.Vector3((b.min.x + b.max.x) / 2, (b.min.y + b.max.y) / 2, b.min.z + size.z / 2),
        type === 'part' ? 'Load Part' : 'Load Modifier');
    } catch (e) {
      toast('volumes_failed', { detail: e instanceof Error ? e.message : String(e) });
    }
  };
  input.click();
}

export async function addPrimitiveObject(shape: Shape) {
  const { min, max, center } = bedBounds(bedArea());
  const geometry = primitiveGeometry(shape, 0.1 * Math.max(max.x - min.x, max.y - min.y));
  const o = { id: newId(), name: shape, geometry, rotX: 0, rotY: 0, scale: [1, 1, 1] as [number, number, number], extruder: 1, settings: {},
    source: new File([await bakeStl({ geometry, rotX: 0, rotY: 0, scale: [1, 1, 1] } as never)], `${shape}.stl`),
    instances: [{ id: newId(), x: center.x, y: center.y, rotZ: 0 }] };
  const spot = nextFreeSpot(getState().objects, o, bedArea()) ?? center;
  o.instances[0] = { ...o.instances[0], x: spot.x, y: spot.y };
  undoable('Add Primitive', () => setState((s) => ({ objects: [...s.objects, o], selected: { object: o.id, instance: o.instances[0].id } })));
}
