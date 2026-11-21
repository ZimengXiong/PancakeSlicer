import { beginBusy } from '../state/busy';
import { Box3, Matrix4, Vector3, type BufferGeometry } from 'three';
import { arrangeSettings } from './arrangeSettings';
import { appendLibrary, firstLayer, plateSignature } from '../slice-input';
import { jobValue } from '../options';
import { responseError } from '../api/access';
import { getState, newId, setState, toast, type PlateObject, type State, type Volume } from '../state/store';
import { undoable } from '../state/history';
import { bakeStl, centred, instanceBox, instanceMatrix, loadMesh, objectMatrix, plateJson, request } from './geometry';
import { bedArea } from '../actions';
import { axisAngle, withRotation } from './rotation';
import { attachFiles, postJob } from '../api/blobs';

let busy = false;

export async function orientJob(o: PlateObject, operation: 'orient' | 'orient-scores'): Promise<Record<string, unknown> | undefined> {
  const s = getState();
  if (!s.api || s.api.mock) throw new Error('Auto-orient requires a connection to the slicing service.');
  const fd = new FormData();
  fd.append('operation', operation);
  await attachFiles(fd, [{ name: 'orient.stl', blob: await bakeStl(o) }]);
  appendLibrary(fd, s, Object.fromEntries(Object.entries(o.settings).map(([key, value]) => [key, jobValue(key, value)])));
  const response = await postJob(fd);
  if (!response.ok) throw await responseError(response);
  const { id } = await response.json() as { id: string };
  try {
    return await new Promise<Record<string, unknown> | undefined>((resolve, reject) => {
      const stop = s.api!.events(id, {
        progress() {},
        done(status) { stop(); resolve(status.record); },
        error(message) { stop(); reject(new Error(message)); },
      });
    });
  } finally {
    await s.api.cancel(id);
  }
}

async function orient(o: PlateObject): Promise<PlateObject> {
  const scores = (await orientJob(o, 'orient-scores'))?.scores;
  if (!Array.isArray(scores)) throw new Error('Oops! We couldn\'t calculate orientation options for this model.');
  const best = (scores as Record<string, unknown>[]).find((x) => (x.object ?? 0) === 0 && x.best === true);
  const axis = best?.axis as number[], angle = Number(best?.angle_deg);
  if (!best || best.original === true || !Array.isArray(axis) || axis.length !== 3 || !Number.isFinite(angle) || Math.abs(angle) < 0.01) return o;
  return withRotation(o, axisAngle(axis, angle));
}

async function splitObject(o: PlateObject): Promise<PlateObject[]> {
  const file = await bakeStl(o);
  const pieces = await new Promise<Blob[]>((resolve, reject) => request({ type: 'split', file }, (m) => {
    if (m.type === 'pieces') resolve(m.pieces);
    else if (m.type === 'error') reject(new Error(m.message));
  }));
  if (pieces.length < 2) throw new Error("Hmm, that object is already a single connected mesh and can't be split further.");
  const result: PlateObject[] = [];
  for (let k = 0; k < pieces.length; k++) {
    const name = `${o.name.replace(/\.(stl|obj)$/i, '')}_${k + 1}`;
    const source = new File([pieces[k]], `${name}.stl`, { type: 'model/stl' });
    const geometry = await loadMesh(source);
    const centre = new Vector3(...geometry.userData.sourceCentre as [number, number, number]);
    const instances = o.instances.map((i) => {
      const m = instanceMatrix(o, i).multiply(objectMatrix(o).invert()).multiply(new Matrix4().makeTranslation(...centre.toArray()));
      const box = new Box3();
      const p = geometry.attributes.position;
      const point = new Vector3();
      for (let v = 0; v < p.count; v++) box.expandByPoint(point.fromBufferAttribute(p, v).applyMatrix4(m));
      const c = box.getCenter(new Vector3());
      return { ...i, id: newId(), x: c.x, y: c.y, z: 0 };
    });
    result.push({ ...o, id: newId(), name, source, geometry, rotX: 0, rotY: 0, scale: [1, 1, 1], instances, settings: { ...o.settings } });
  }
  return result;
}

async function splitParts(o: PlateObject): Promise<PlateObject[]> {
  if (o.volumes?.length) throw new Error('Select a single-part object to split it into separate parts.');
  const file = await bakeStl(o, new Matrix4());
  const blobs = await new Promise<Blob[]>((resolve, reject) => request({ type: 'split', file }, (m) => {
    if (m.type === 'pieces') resolve(m.pieces);
    else if (m.type === 'error') reject(new Error(m.message));
  }));
  if (blobs.length < 2) throw new Error("Hmm, that object is already a single connected mesh and can't be split further.");
  const base = o.name.replace(/\.(stl|obj|3mf|stp|step)$/i, '');
  const files = [...blobs].sort((a, b) => b.size - a.size).map((blob, k) => new File([blob], `${base}_${k + 1}.stl`, { type: 'model/stl' }));
  const pieces = await Promise.all(files.map((f) => loadMesh(f)));
  const centre = (g: BufferGeometry) => new Vector3(...g.userData.sourceCentre as [number, number, number]);
  const c0 = centre(pieces[0]);
  const volumes: Volume[] = pieces.slice(1).map((g, k) => {
    const [geometry, back] = centred(g);
    return { id: newId(), name: `${base}_${k + 2}`, type: 'part', geometry,
      matrix: new Matrix4().makeTranslation(...centre(g).sub(c0).toArray()).multiply(back).elements, extruder: 0, settings: {} };
  });
  const next: PlateObject = { ...o, geometry: pieces[0], source: files[0], volumes, paint: undefined };
  return [{ ...next, instances: o.instances.map((i) => {
    const before = c0.clone().applyMatrix4(instanceMatrix(o, i)), after = new Vector3().applyMatrix4(instanceMatrix(next, i));
    return { ...i, x: i.x + before.x - after.x, y: i.y + before.y - after.y, z: (i.z ?? 0) + before.z - after.z };
  }) }];
}

export async function run(operation: 'orient' | 'split' | 'split-parts', all: boolean) {
  const s = getState();
  if (busy || s.project || s.tab !== 'prepare') return;
  const plateWide = operation === 'orient' && (all || !s.selected);
  if (plateWide && s.projectPlates.find((p) => p.index === s.plateIndex)?.locked) return toast('plate_locked');
  const objects = plateWide ? s.objects : s.objects.filter((o) => o.id === s.selected?.object);
  if (!objects.length) return;
  busy = true;
  const signature = plateSignature(s);
  const created: PlateObject[] = [];
  const overlay = beginBusy(operation === 'orient' ? 'Orienting objects' : 'Splitting object');
  try {
    setState({ toast: null });
    const replacements = new Map<number, PlateObject[]>();
    for (const o of objects) {
      const next = operation === 'orient' ? [await orient(o)] : operation === 'split' ? await splitObject(o) : await splitParts(o);
      replacements.set(o.id, next);
      if (operation === 'split') created.push(...next);
    }
    if (signature !== plateSignature(getState())) throw new Error('The plate changed while that tool was running. Give it another try!');
    const next = s.objects.flatMap((o) => replacements.get(o.id) ?? [o]);
    const first = replacements.get(objects[0].id)![0];
    undoable(operation === 'orient' ? 'Orient' : operation === 'split' ? 'Split to Objects' : 'Split to Parts',
      () => setState({ objects: next, selected: { object: first.id, instance: first.instances[0].id }, toast: null }));
  } catch (error) {
    created.forEach((o) => o.geometry.dispose());
    toast('operations_failed', { detail: error instanceof Error ? error.message : String(error) });
  } finally {
    busy = false;
    overlay.end();
  }
}

export async function plateForm(s: State, operation: string): Promise<FormData> {
  const { plate, files } = await plateJson(s.objects, firstLayer(s));
  const fd = new FormData();
  fd.append('operation', operation);
  await attachFiles(fd, files);
  fd.append('plate', JSON.stringify(plate));
  appendLibrary(fd, s);
  return fd;
}

interface ArrangeResult { schema: string; plates: number; unplaced: number; warnings?: { code?: string; message: string }[]; objects: { instances: { plate: number | null; x?: number; y?: number; rot?: number }[] }[] }

export async function arrangeNative() {
  const s = getState();
  if (busy || !s.api || s.api.mock || s.project || !s.objects.length) return;
  busy = true;
  const overlay = beginBusy('Arranging');
  try {
    const fd = await plateForm(s, 'arrange');
    const { distance, rotation, multiMaterials } = arrangeSettings();
    if (distance > 0) fd.append('arrange_spacing', String(distance));
    fd.append('arrange_rotation', rotation ? '1' : '0');
    fd.append('arrange_multi_materials', multiMaterials ? '1' : '0');
    const response = await postJob(fd);
    if (!response.ok) throw await responseError(response);
    const { id } = await response.json() as { id: string };
    let result: ArrangeResult;
    try {
      result = await new Promise<ArrangeResult>((resolve, reject) => {
        const stop = s.api!.events(id, {
          progress() {},
          done(status) { stop(); resolve(status.record as unknown as ArrangeResult); },
          error(message) { stop(); reject(new Error(message)); },
        });
      });
    } finally {
      await s.api.cancel(id);
    }
    if (result?.schema !== 'orca.arrange.v1' || result.objects?.length !== s.objects.length) throw new Error('Oops! We couldn\'t calculate a plate arrangement.');
    if (getState().objects !== s.objects) return;
    const area = bedArea(), xs = area.map((p) => p.x), width = Math.max(...xs) - Math.min(...xs);
    let away = 0;
    const objects = s.objects.map((o, k) => ({ ...o, instances: o.instances.map((inst, m) => {
      const r = result.objects[k].instances[m];
      if (!r || r.plate === null || r.x === undefined || r.y === undefined) { away++; return inst; }
      if (r.plate > 1) away++;
      const turned = { ...inst, rotZ: r.rot ?? inst.rotZ };
      const b = instanceBox(o, turned), cx = (b.min.x + b.max.x) / 2, cy = (b.min.y + b.max.y) / 2;
      return { ...turned, x: turned.x + r.x - cx + (r.plate - 1) * width * 1.2, y: turned.y + r.y - cy };
    }) }));
    undoable('Arrange', () => setState({ objects }));
    if (away) toast('copyx_couldnt_fit_plate', { away, value: away > 1 ? 's do' : ' does' });
    const clearance = (result.warnings ?? []).filter((w) => w.code === 'nonplanar_head_clearance' && w.message);
    if (clearance.length) {
      const detail = clearance[0].message + (clearance.length > 1 ? `\n…and ${clearance.length - 1} more clearance warning${clearance.length > 2 ? 's' : ''}.` : '');
      toast('arrange_nonplanar_clearance', { detail });
    }
  } catch (e) {
    toast('couldnt_auto_arrange_plate', { detail: e instanceof Error ? e.message : e });
  } finally {
    busy = false;
    overlay.end();
  }
}
