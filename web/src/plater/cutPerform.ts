import { beginBusy } from '../state/busy';
import * as THREE from 'three';
import { bedArea } from '../actions';
import { responseError } from '../api/access';
import { appendLibrary, plateSignature } from '../slice-input';
import { undoable } from '../state/history';
import { getState, newId, setState, toast, type Instance, type PlateObject, type Volume } from '../state/store';
import { bakeStl, centred, instanceBox, instanceMatrix, loadMesh, nearestFreeSpot, objectMatrix } from './geometry';
import { canPerformCut, cutNormal, cutPoint, cutTarget } from './cut';
import { attachFiles, postJob } from '../api/blobs';

interface Piece { side: 'upper' | 'lower' | `dowel-${number}`; triangles: number; open_edges: number; volume_mm3: number }

let busy = false;

export async function performCut() {
  const s = getState();
  const t = cutTarget(s);
  const c = s.cut;
  if (busy || !t || !c || !s.api || s.api.mock || !canPerformCut(s)) return;
  const { o, inst } = t;
  busy = true;
  const signature = plateSignature(s);
  const created: PlateObject[] = [], overlay = beginBusy('Cutting object');
  try {
    setState({ toast: null });
    const toStl = instanceMatrix(o, inst).multiply(objectMatrix(o).invert()).invert();
    const p = cutPoint(s, c)!.applyMatrix4(toStl);
    const n = cutNormal(c).transformDirection(toStl);
    if (n.z < -1 + 1e-9) n.set(1e-5, 0, -1).normalize();
    const joints = c.mode === 'dovetail' || c.connectors?.length ? await import('./cutJoints') : null;
    const fd = new FormData();
    fd.append('operation', 'cut');
    fd.append('cut', [p.x, p.y, p.z, n.x, n.y, n.z].map((v) => v.toFixed(6)).join(','));
    fd.append('cut_keep', c.toParts || c.keepUpper && c.keepLower ? 'both' : c.keepUpper ? 'upper' : 'lower');
    if (c.placeLower && !c.toParts) fd.append('cut_place_lower', '1');
    for (const [k, v] of Object.entries(joints?.jointFields(s, c, toStl, p, n) ?? {})) fd.append(k, v);
    await attachFiles(fd, [{ name: 'cut.stl', blob: await bakeStl(o) }]);
    appendLibrary(fd, s);
    const response = await postJob(fd);
    if (!response.ok) throw await responseError(response);
    const { id } = await response.json() as { id: string };
    let files: { piece: Piece; file: File }[];
    try {
      const pieces = await new Promise<Piece[]>((resolve, reject) => {
        const stop = s.api!.events(id, {
          progress() {},
          done(status) { stop(); resolve((status.record?.pieces ?? []) as Piece[]); },
          error(message) { stop(); reject(new Error(message)); },
        });
      });
      if (!pieces.length) throw new Error('Oops! Cutting didn\'t produce any parts.');
      const base = o.name.replace(/\.(stl|obj)$/i, '');
      files = await Promise.all(pieces.map(async (piece) => {
        const r = await fetch(`${s.api!.downloadUrl(id)}?piece=${piece.side}`);
        if (!r.ok) throw await responseError(r);
        const name = piece.side.startsWith('dowel-') ? `${base}-Dowel-Connector-${piece.side.slice(6)}` : pieces.length > 1 ? `${base}_${piece.side === 'upper' ? 'A' : 'B'}` : base;
        return { piece, file: new File([await r.blob()], `${name}.stl`, { type: 'model/stl' }) };
      }));
    } finally {
      await s.api.cancel(id);
    }
    const turned = (side: Piece['side']) => (side === 'upper' && (c.rotX !== 0 || c.rotY !== 0 || !!c.flipUpper)) || (side === 'lower' && c.placeLower);
    if (c.toParts) return void await cutToParts(o, files, signature);
    const others = s.objects.filter((ob) => ob.id !== o.id);
    const placed: PlateObject[] = [];
    for (const side of ['lower', 'upper', ...files.map((f) => f.piece.side).filter((x) => x.startsWith('dowel-'))] as Piece['side'][]) {
      const f = files.find((x) => x.piece.side === side);
      if (!f) continue;
      const geometry = await loadMesh(f.file);
      const centre = new THREE.Vector3(...geometry.userData.sourceCentre as [number, number, number]);
      const piece: PlateObject = { ...o, project: undefined, id: newId(), name: f.file.name.replace(/\.stl$/, ''), source: f.file, geometry, rotX: side === 'upper' && c.flipUpper ? 180 : 0, rotY: 0,
        scale: [1, 1, 1], settings: { ...o.settings }, instances: [] };
      for (const i of o.instances) {
        const m = instanceMatrix(o, i).multiply(objectMatrix(o).invert()).multiply(new THREE.Matrix4().makeTranslation(centre.x, centre.y, centre.z));
        const box = new THREE.Box3().setFromBufferAttribute(geometry.attributes.position as THREE.BufferAttribute).applyMatrix4(m);
        let at = box.getCenter(new THREE.Vector3());
        const moved: Instance = { ...i, id: newId(), x: at.x, y: at.y, z: 0 };
        if (turned(side) || (side === 'upper' && files.length > 1) || side.startsWith('dowel-')) {
          const size = instanceBox({ ...piece, instances: [moved] }, moved).getSize(new THREE.Vector3());
          const spot = nearestFreeSpot([...others, ...placed, piece], new THREE.Vector2(size.x + 4, size.y + 4), new THREE.Vector2(at.x, at.y), bedArea());
          if (spot) at = new THREE.Vector3(spot.x, spot.y, 0);
          moved.x = at.x;
          moved.y = at.y;
        }
        piece.instances.push(moved);
      }
      created.push(piece);
      placed.push(piece);
    }
    if (signature !== plateSignature(getState())) throw new Error('The plate changed while we were cutting the model. Please try the cut again.');
    const pieces = created.slice().reverse();
    const objects = s.objects.flatMap((ob) => (ob.id === o.id ? pieces : [ob]));
    const open = files.filter((f) => f.piece.open_edges > 0);
    undoable('Cut by Plane', () => setState({ objects, selected: { object: pieces[0].id, instance: pieces[0].instances[0].id, allInstances: true },
      gizmo: 'none', cut: null, toast: open.length ? `${open.map((f) => f.file.name).join(', ')}: open edges after the cut` : null, toastKind: null }));
  } catch (error) {
    created.forEach((x) => x.geometry.dispose());
    toast('cutperform_failed', { detail: error instanceof Error ? error.message : String(error) });
  } finally {
    busy = false;
    overlay.end();
  }
}

async function cutToParts(o: PlateObject, files: { piece: Piece; file: File }[], signature: string) {
  const s = getState(), inst = o.instances[0];
  const load = async (side: Piece['side']) => {
    const f = files.find((x) => x.piece.side === side);
    if (!f) throw new Error('The cut plane didn\'t slice through the model, so it stayed in one piece.');
    const geometry = await loadMesh(f.file);
    const toWorld = instanceMatrix(o, inst).multiply(objectMatrix(o).invert())
      .multiply(new THREE.Matrix4().makeTranslation(...geometry.userData.sourceCentre as [number, number, number]));
    return { f, geometry, toWorld };
  };
  const lower = await load('lower'), upper = await load('upper');
  const box = new THREE.Box3().setFromBufferAttribute(lower.geometry.attributes.position as THREE.BufferAttribute).applyMatrix4(lower.toWorld);
  const at = box.getCenter(new THREE.Vector3());
  const base: PlateObject = { ...o, project: undefined, id: newId(), name: o.name.replace(/\.(stl|obj)$/i, ''), source: lower.f.file, geometry: lower.geometry,
    rotX: 0, rotY: 0, scale: [1, 1, 1], settings: { ...o.settings }, volumes: undefined, paint: undefined, layerProfile: undefined,
    instances: [{ ...inst, id: newId(), x: at.x, y: at.y, z: 0 }] };
  const [geometry, back] = centred(upper.geometry);
  const part: Volume = { id: newId(), name: `${base.name}_A`, type: 'part', geometry, extruder: 0, settings: {},
    matrix: instanceMatrix(base, base.instances[0]).invert().multiply(upper.toWorld).multiply(back).elements };
  const object: PlateObject = { ...base, volumes: [part] };
  if (signature !== plateSignature(getState())) throw new Error('The plate changed while we were cutting the model. Please try the cut again.');
  undoable('Cut by Plane', () => setState({ objects: s.objects.flatMap((ob) => (ob.id === o.id ? [object] : [ob])),
    selected: { object: object.id, instance: object.instances[0].id, allInstances: true }, gizmo: 'none', cut: null, toast: null }));
}
