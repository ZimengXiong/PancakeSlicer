import type * as THREE from 'three';
import { loadMesh, loadProject, placeholderGeometry, volumeGeometry } from '../plater/geometry';
import type { ProjectPlate } from '../plater/threemf';
import type { Doc } from '../state/history';
import { reserveIds, type NativeObject, type PlateObject, type State, type Volume } from '../state/store';
import { newKey, type StoredDocBase } from './backend';
import { movePlateSetup } from '../state/plate-setup';
import { documentStrip } from '../profiles/slots';

type StoredVolume = Omit<Volume, 'geometry'> & { geometry: { position: Float32Array; normal?: Float32Array; index: Uint32Array | Uint16Array | null } };
type StoredObject = Omit<PlateObject, 'geometry' | 'source' | 'volumes' | 'native'> & { source: string; volumes?: StoredVolume[];
  native?: Omit<NativeObject, 'files'> & { files: Record<string, string> };
  simplification?: { trianglesBefore: number; trianglesAfter: number; maxErrorMm: number; kept: 'simplified' | 'original' } };
export type StoredDoc = Omit<Doc, 'objects' | 'projectPlates' | 'project' | 'projectSlots'> & StoredDocBase & {
  objects: StoredObject[];
  projectPlates: (ProjectPlate & { objects: StoredObject[] })[];
  project: string | null;
};

const blobKeys = new WeakMap<Blob, string>();
const keyOf = (b: Blob) => blobKeys.get(b) ?? (blobKeys.set(b, newKey()), blobKeys.get(b)!);
export function assignBlobKey(blob: Blob, key: string) {
  if (!blobKeys.has(blob)) blobKeys.set(blob, key);
}

export const plateCount = (d: Pick<State, 'objects' | 'projectPlates' | 'plateIndex'>) =>
  d.objects.length + d.projectPlates.filter((p) => p.index !== d.plateIndex).reduce((n, p) => n + p.objects.length, 0);
export const nameOf = (doc: Doc) => (doc.projectName ?? doc.project?.name ?? doc.objects[0]?.name ?? 'Untitled').replace(/\.(stl|obj|3mf|drc|step|stp)$/i, '');

export function encode(doc: Doc): { stored: StoredDoc; blobs: Map<string, Blob> } {
  const blobs = new Map<string, Blob>();
  const keep = (b: Blob) => { const k = keyOf(b); blobs.set(k, b); return k; };
  const volume = ({ geometry: g, ...rest }: Volume): StoredVolume => ({ ...rest, geometry: {
    position: g.attributes.position.array as Float32Array, normal: g.attributes.normal?.array as Float32Array | undefined,
    index: (g.index?.array as Uint32Array | undefined) ?? null } });
  const object = ({ geometry: _, source, volumes, native, ...rest }: PlateObject): StoredObject =>
    ({ ...rest, source: keep(source), ...(volumes ? { volumes: volumes.map(volume) } : {}),
      ...(native ? { native: { ...native, files: Object.fromEntries(Object.entries(native.files).map(([k, b]) => [k, keep(b)])) } } : {}) });
  const { projectSlots, ...rest } = doc;
  const stored = {
    ...rest,
    ...(projectSlots ? { filamentSlots: projectSlots.filaments, slotColours: projectSlots.colours, mixes: projectSlots.mixes } : {}),
    objects: doc.objects.map(object),
    projectPlates: doc.projectPlates.map((p) => ({ ...p, objects: p.objects.map(object) })),
    project: doc.project && keep(doc.project),
  };
  return { stored: { ...stored, files: Object.fromEntries([...blobs].map(([k, b]) => [k, b instanceof File ? b.name : k])) }, blobs };
}

let drawThumbnail: (() => string | null) | null = null;
export const registerThumbnail = (draw: () => string | null) => { drawThumbnail = draw; };
export const thumbnail = (): string | null => drawThumbnail?.() ?? null;

export async function decode(doc: StoredDoc, get: (key: string) => Promise<Blob | null>): Promise<Partial<State>> {
  const blobs = new Map(await Promise.all(Object.entries(doc.files ?? {}).map(async ([k, name]) => {
    const b = await get(k);
    if (!b) throw new Error('a model file is missing');
    const file = b instanceof File && (!name || b.name === name) ? b : new File([b], name || k);
    blobKeys.set(file, k);
    return [k, file] as const;
  })));
  const meshes = new Map<string, THREE.BufferGeometry>();
  const items = new Map<number, { geometry: THREE.BufferGeometry; centre: THREE.Vector3 }>();
  if (doc.project) {
    let list: { buildIndex: number }[] = [];
    await loadProject(blobs.get(doc.project) as File, {
      items: (it) => { list = it; },
      mesh: (i, geometry, centre) => items.set(list[i].buildIndex, { geometry, centre }),
      missing: () => {},
    });
  }
  const all = [...doc.objects, ...doc.projectPlates.flatMap((p) => p.objects)];
  const explode = all.some((o) => o.native) ? await import('../plater/explode') : null;
  const natives = new Map<StoredObject, NativeObject>();
  for (const o of all) {
    if (!o.native) continue;
    const native = { ...o.native, files: Object.fromEntries(Object.entries(o.native.files).map(([k, key]) => [k, blobs.get(key) as Blob])) };
    natives.set(o, native);
    const main = String((native.entry.volumes as { file: string }[])[native.main].file);
    if (o.source === o.native.files[main] && !meshes.has(o.source)) meshes.set(o.source, await explode!.nativeGeometry(native));
  }
  const simplify = (o: StoredObject) => !o.native && o.simplification?.kept !== 'original' &&
    (!!o.simplification || !Object.values(o.paint ?? {}).some(Boolean) && !o.volumes?.some((v) => Object.values(v.paint ?? {}).some(Boolean)));
  const meshKey = (o: StoredObject) => o.native ? o.source : `${o.source}/${simplify(o) ? 'simplified' : 'original'}`;
  const loadStoredMesh: (file: File, progress?: (pct: number) => void, simplify?: boolean) => Promise<THREE.BufferGeometry> = loadMesh;
  for (const o of all)
    if (!o.project && !meshes.has(meshKey(o)))
      meshes.set(meshKey(o), await loadStoredMesh(blobs.get(o.source) as File, undefined, simplify(o)));
  const revive = (stored: StoredObject): PlateObject => {
    const { volumes, native: _stored, ...o } = stored;
    const source = blobs.get(o.source) as File;
    const native = natives.get(stored);
    if (!o.project) return { ...o, native, source, geometry: meshes.get(meshKey(stored))!,
      simplification: meshes.get(meshKey(stored))!.userData.simplification ?? o.simplification,
      ...(volumes ? { volumes: volumes.map((v) => {
        const geometry = v.native !== undefined && explode ? explode.markPristine(explode.rawArrays(v.geometry.position, v.geometry.index))
          : volumeGeometry(v.geometry.position, v.geometry.index, v.geometry.normal);
        return { ...v, geometry };
      }) } : {}) };
    const item = o.project.buildIndex === undefined ? undefined : items.get(o.project.buildIndex);
    if (!item) return { ...o, source, geometry: placeholderGeometry() };
    if (!o.project.placeholder) return { ...o, source, geometry: item.geometry };
    const { x, y, z } = item.centre;
    return { ...o, source, geometry: item.geometry, project: { ...o.project, placeholder: false, origin: [x, y, z] },
      instances: [{ ...o.instances[0], x, y, z }, ...o.instances.slice(1)] };
  };
  const objects = doc.objects.map(revive);
  const projectPlates = doc.projectPlates.map((p) => ({ ...p, objects: p.objects.map(revive) }));
  reserveIds(Math.max(0, ...[...objects, ...projectPlates.flatMap((p) => p.objects)].flatMap((o) => [o.id, ...o.instances.map((i) => i.id), ...(o.volumes ?? []).map((v) => v.id)])));
  const { files: _files, ...fields } = doc;
  const { filamentSlots: _f, slotColours: _c, mixes: _m, projectSlots: _p, ...state } = fields as typeof fields & { filamentSlots?: unknown; slotColours?: unknown; mixes?: unknown; projectSlots?: unknown };
  const own = documentStrip(doc as Parameters<typeof documentStrip>[0]);
  const { plateSetup, ...moved } = movePlateSetup(state as typeof state & { bedType?: string; flush?: { matrix?: number[] | null } });
  void import('../profiles/plate-setup-move').then((m) => m.adoptStoredPlateSetup(plateSetup, String(doc.printer ?? ''), doc.projectName ?? 'This project'));
  return { ...moved, processVariant: doc.processVariant ?? null, objects, projectPlates, project: doc.project ? (blobs.get(doc.project) as File) : null,
    projectSlots: own ? { filaments: own.filaments, colours: own.colours, mixes: own.mixes } : null };
}
