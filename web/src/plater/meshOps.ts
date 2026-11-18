import { beginBusy } from '../state/busy';
import * as THREE from 'three';
import { responseError } from '../api/access';
import { getState, logNotice, markStale, setState, toast, type PlateObject } from '../state/store';
import { undoable } from '../state/history';
import { bakeStl, instanceBox, instanceMatrix, loadMesh } from './geometry';
import { appendLibrary } from '../slice-input';
import { attachFiles, postJob } from '../api/blobs';

export async function toolJob(operation: string, fields: Record<string, string>, files: [Blob, string][]) {
  const s = getState();
  if (!s.api || s.api.mock) throw new Error('This action needs a connection to the slicing service.');
  const fd = new FormData();
  fd.append('operation', operation);
  for (const [k, v] of Object.entries(fields)) fd.append(k, v);
  appendLibrary(fd, s);
  await attachFiles(fd, files.map(([blob, name]) => ({ name, blob })));
  const response = await postJob(fd);
  if (!response.ok) throw await responseError(response);
  const { id } = await response.json() as { id: string };
  try {
    const record = await new Promise<Record<string, unknown>>((resolve, reject) => {
      const stop = s.api!.events(id, { progress() {}, done(st) { stop(); resolve((st.record ?? {}) as Record<string, unknown>); }, error(m) { stop(); reject(new Error(m)); } });
    });
    const r = await fetch(s.api.downloadUrl(id));
    if (!r.ok) throw await responseError(r);
    return { record, blob: await r.blob() };
  } finally {
    await s.api.cancel(id);
  }
}

export async function meshJob(operation: string, fields: Record<string, string>, files: [Blob, string][], name: string) {
  const { record, blob } = await toolJob(operation, fields, files);
  const file = new File([blob], `${name.replace(/\.(stl|obj|3mf|drc|step|stp)$/i, '')}.stl`, { type: 'model/stl' });
  return { record, file, geometry: await loadMesh(file) };
}

const run = async (o: PlateObject, operation: 'repair' | 'simplify', fields: Record<string, string> = {}) =>
  meshJob(operation, fields, [[await bakeStl({ ...o, rotX: 0, rotY: 0, scale: [1, 1, 1] }, new THREE.Matrix4()), 'mesh.stl']], o.name);

function replace(o: PlateObject, file: File, geometry: THREE.BufferGeometry, name: string) {
  const c = new THREE.Vector3(...(geometry.userData.sourceCentre as [number, number, number] ?? [0, 0, 0]));
  geometry.computeBoundingBox();
  const next = { ...o, geometry, source: file, paint: undefined };
  next.instances = o.instances.map((i) => {
    const was = geometry.boundingBox!.clone().applyMatrix4(instanceMatrix(o, i).multiply(new THREE.Matrix4().makeTranslation(c.x, c.y, c.z)));
    const now = instanceBox({ ...next, instances: [i] }, i);
    const d = was.getCenter(new THREE.Vector3()).sub(now.getCenter(new THREE.Vector3()));
    return { ...i, x: i.x + d.x, y: i.y + d.y };
  });
  undoable(name, () => setState((st) => ({ objects: st.objects.map((x) => (x.id === o.id ? next : x)), job: markStale(st.job) })));
}

export async function fixModel(objectId: number) {
  const o = getState().objects.find((x) => x.id === objectId);
  if (!o) return;
  const busy = beginBusy('Repairing model object');
  try {
    const { record, file, geometry } = await run(o, 'repair');
    const counts = (record.repair_counts ?? {}) as Record<string, number>;
    const done = Object.entries(counts).filter(([, n]) => n > 0).map(([k, n]) => `${n} ${k.replace(/_/g, ' ')}`);
    const closed = Number(record.boundary_edges_closed) || 0;
    if (closed) done.push(`${closed} boundary edges closed`);
    replace(o, file, geometry, 'Fix Model');
    if (done.length) logNotice('model_repaired', { name: o.name, done: done.join(', ') });
    else logNotice('model_no_errors', { name: o.name });
  } catch (e) {
    toast('couldnt_repair_model', { detail: e instanceof Error ? e.message : String(e) });
  } finally {
    busy.end();
  }
}

export async function simplifyModel(objectId: number, triangles: number, maxError?: number) {
  const o = getState().objects.find((x) => x.id === objectId);
  if (!o) return;
  const busy = beginBusy('Simplifying model');
  try {
    const { record, file, geometry } = await run(o, 'simplify', maxError ? { simplify_max_error: String(maxError) } : { simplify_triangles: String(Math.max(1, Math.round(triangles))) });
    replace(o, file, geometry, 'Simplify Model');
    toast('triangles', { name: o.name, trianglesbefore: record.triangles_before, trianglesafter: record.triangles_after });
  } catch (e) {
    toast('simplify_model', { detail: e instanceof Error ? e.message : String(e) });
  } finally {
    busy.end();
  }
}

export const triangleCount = (o: PlateObject) => (o.geometry.index ? o.geometry.index.count : o.geometry.attributes.position.count) / 3;
