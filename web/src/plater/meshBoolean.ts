import { beginBusy } from '../state/busy';
import * as THREE from 'three';
import { getState, markStale, setState, toast, type PlateObject, type Volume } from '../state/store';
import { undoable } from '../state/history';
import { bakeStl, instanceMatrix, volumeGeometry } from './geometry';
import { meshJob } from './meshOps';

export type BooleanOp = 'union' | 'difference' | 'intersection';
export type PartKey = 'main' | number;

export const BOOLEAN_WARNING = 'Oops! Couldn\'t combine or subtract those parts. Make sure the meshes overlap cleanly.';

const partStl = (o: PlateObject, k: PartKey) => {
  const v = o.volumes?.find((x) => x.id === k);
  return v ? bakeStl({ ...o, geometry: v.geometry }, new THREE.Matrix4().fromArray(v.matrix)) : bakeStl(o, new THREE.Matrix4());
};

function withMain(o: PlateObject, geometry: THREE.BufferGeometry, source: File, c: THREE.Vector3): PlateObject {
  const back = new THREE.Matrix4().makeTranslation(-c.x, -c.y, -c.z);
  const next: PlateObject = { ...o, geometry, source, paint: undefined, brimEars: undefined, volumes: o.volumes?.map((v) => ({ ...v, matrix: back.clone().multiply(new THREE.Matrix4().fromArray(v.matrix)).elements })) };
  next.instances = o.instances.map((i) => {
    const was = c.clone().applyMatrix4(instanceMatrix(o, i)), now = new THREE.Vector3().applyMatrix4(instanceMatrix(next, i));
    return { ...i, x: i.x + was.x - now.x, y: i.y + was.y - now.y, z: (i.z ?? 0) + was.z - now.z };
  });
  return next;
}

export async function meshBoolean(objectId: number, op: BooleanOp, a: PartKey, b: PartKey, deleteInput: boolean) {
  const o = getState().objects.find((x) => x.id === objectId);
  if (!o || a === b) return;
  const busy = beginBusy('Mesh Boolean');
  try {
    const { record, file, geometry } = await meshJob('boolean', { boolean_op: op }, [[await partStl(o, a), 'a.stl'], [await partStl(o, b), 'b.stl']], o.name);
    if (!geometry.attributes.position.count) throw new Error(BOOLEAN_WARNING);
    const c = new THREE.Vector3(...(geometry.userData.sourceCentre as [number, number, number] ?? [0, 0, 0]));
    const remove = op === 'union' || deleteInput;
    const [src, tool] = remove && b === 'main' ? [b, a] : [a, b];
    const live = getState().objects.find((x) => x.id === objectId) ?? o;
    let next: PlateObject;
    if (src === 'main') next = withMain(live, geometry, file, c);
    else {
      const v = live.volumes!.find((x) => x.id === src)!;
      geometry.computeBoundingBox();
      const d = geometry.boundingBox!.getCenter(new THREE.Vector3()).add(c);
      const centred = volumeGeometry((geometry.attributes.position.array as Float32Array).slice(), (geometry.index?.array as Uint32Array | undefined)?.slice() ?? null, (geometry.attributes.normal?.array as Float32Array | undefined)?.slice());
      const result: Volume = { ...v, geometry: centred, matrix: new THREE.Matrix4().makeTranslation(d.x, d.y, d.z).elements, paint: undefined, text: undefined, name: `${v.name} - ${op}` };
      next = { ...live, volumes: live.volumes!.map((x) => (x.id === src ? result : x)) };
    }
    if (remove) next = { ...next, volumes: next.volumes?.filter((x) => x.id !== tool) };
    undoable('Mesh Boolean', () => setState((st) => ({ objects: st.objects.map((x) => (x.id === objectId ? next : x)), job: markStale(st.job) })));
    if (record.best_effort) toast('mesh_boolean_finished_best', { value: (record.preflight_reasons as string[] | undefined)?.length ? ` (${(record.preflight_reasons as string[]).join(', ')})` : '' });
  } catch (e) {
    toast('meshboolean_failed', { warning: BOOLEAN_WARNING, detail: e instanceof Error ? e.message : String(e) });
  } finally {
    busy.end();
  }
}
