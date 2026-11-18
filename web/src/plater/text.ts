import * as THREE from 'three';
import { getState, newId, setState, toast, type PlateObject, type TextSpec, type Volume, type VolumeType } from '../state/store';
import { undoable } from '../state/history';
import { bedArea } from '../actions';
import { bakeStl, bedBounds, nextFreeSpot, volumeGeometry } from './geometry';
import { meshJob } from './meshOps';
import { placeVolume } from './volumes';

export const TEXT_DEFAULT: TextSpec = { text: 'Embossed text', font: '', size: 10, depth: 1, bold: false, italic: false, lineSpacing: 1, charGap: 0, align: 'center' };

export interface Font { id: string; name: string }
let fonts: Promise<Font[]> | null = null;
export function loadFonts(): Promise<Font[]> {
  fonts ??= fetch('/api/emboss-fonts').then((r) => (r.ok ? r.json() : { fonts: [] })).then((d: { fonts: Font[] }) => d.fonts).catch(() => []);
  return fonts;
}

const fields = (t: TextSpec): Record<string, string> => ({
  text: t.text, ...(t.font ? { font: t.font } : {}), size: String(t.size), depth: String(t.depth), line_spacing: String(t.lineSpacing),
  char_gap: String(t.charGap), align: t.align, ...(t.bold ? { bold: '1' } : {}), ...(t.italic ? { italic: '1' } : {}),
});

async function surfaceMesh(o: PlateObject, type: VolumeType, t: TextSpec) {
  const { geometry } = await meshJob('text', { ...fields(t), ...surfaceFields(t.surface!, type) }, [[await bakeStl(o, new THREE.Matrix4()), 'target.stl']], 'text');
  return inFrame(geometry);
}

export const surfaceFields = ({ at, normal }: NonNullable<TextSpec['surface']>, type: VolumeType): Record<string, string> =>
  ({ use_surface: '1', at: at.join(','), normal: normal.join(','), ...(type === 'part' ? { outside: '1' } : {}) });

export function inFrame(geometry: THREE.BufferGeometry) {
  geometry.computeBoundingBox();
  const c = new THREE.Vector3(...(geometry.userData.sourceCentre as [number, number, number] ?? [0, 0, 0]));
  const d = geometry.boundingBox!.getCenter(new THREE.Vector3()).add(c);
  const centred = volumeGeometry((geometry.attributes.position.array as Float32Array).slice(), (geometry.index?.array as Uint32Array | undefined)?.slice() ?? null, (geometry.attributes.normal?.array as Float32Array | undefined)?.slice());
  return { centred, matrix: new THREE.Matrix4().makeTranslation(d.x, d.y, d.z).elements };
}

async function textMesh(t: TextSpec) {
  const { file, geometry } = await meshJob('text', fields(t), [], 'text');
  const g = volumeGeometry((geometry.attributes.position.array as Float32Array).slice(), (geometry.index?.array as Uint32Array | undefined)?.slice() ?? null, (geometry.attributes.normal?.array as Float32Array | undefined)?.slice());
  return { file, geometry, centred: g };
}

const nameOf = (t: TextSpec) => t.text.split('\n')[0].slice(0, 40) || 'Text';

let busy = false;
async function withService<T>(work: () => Promise<T>): Promise<T | undefined> {
  if (busy) return undefined;
  busy = true;
  try {
    return await work();
  } catch (e) {
    toast('text', { detail: e instanceof Error ? e.message : String(e) });
    return undefined;
  } finally {
    busy = false;
  }
}

export async function addText(objectId: number | null, type: VolumeType = 'part', spec: TextSpec = TEXT_DEFAULT) {
  await withService(async () => {
    const { file, geometry, centred } = await textMesh(spec);
    if (objectId !== null) {
      const id = placeVolume(objectId, type, nameOf(spec), centred,
        (b, size) => new THREE.Vector3((b.min.x + b.max.x) / 2, (b.min.y + b.max.y) / 2, type === 'part' ? b.max.z + size.z / 2 : b.max.z - size.z / 2),
        'Add text', { text: spec });
      const s = getState(), o = s.objects.find((x) => x.id === objectId);
      if (id !== undefined && o) setState({ selected: { object: o.id, instance: s.selected?.instance ?? o.instances[0].id, volume: id }, gizmo: 'text' });
      return;
    }
    const { center } = bedBounds(bedArea());
    const o: PlateObject = { id: newId(), name: nameOf(spec), geometry, rotX: 0, rotY: 0, scale: [1, 1, 1], extruder: 1, settings: {}, source: file, text: spec,
      instances: [{ id: newId(), x: center.x, y: center.y, rotZ: 0 }] };
    const spot = nextFreeSpot(getState().objects, o, bedArea()) ?? center;
    o.instances[0] = { ...o.instances[0], x: spot.x, y: spot.y };
    undoable('Add text', () => setState((s) => ({ objects: [...s.objects, o], selected: { object: o.id, instance: o.instances[0].id }, gizmo: 'text' })));
  });
}

export function editedText(s = getState()): { object: PlateObject; volume?: Volume; spec: TextSpec } | null {
  const o = s.objects.find((x) => x.id === s.selected?.object);
  if (!o) return null;
  const v = s.selected?.volume !== undefined ? o.volumes?.find((x) => x.id === s.selected!.volume) : undefined;
  if (v?.text) return { object: o, volume: v, spec: v.text };
  return o.text && !v ? { object: o, spec: o.text } : null;
}

export function placeOnSurface(at: [number, number, number], normal: [number, number, number]) {
  const t = editedText();
  if (t?.volume) void updateText({ ...t.spec, surface: { at, normal } });
}

export async function updateText(spec: TextSpec) {
  const target = editedText();
  if (!target || !spec.text.trim()) return;
  await withService(async () => {
    const { object, volume } = target;
    if (volume && spec.surface) {
      const { centred, matrix } = await surfaceMesh(object, volume.type, spec);
      undoable('Edit text', () => setState((s) => ({
        objects: s.objects.map((o) => (o.id !== object.id ? o : { ...o, volumes: o.volumes?.map((v) => (v.id === volume.id ? { ...v, geometry: centred, matrix, text: spec, name: nameOf(spec) } : v)) })),
      })));
      return;
    }
    const { file, geometry, centred } = await textMesh(spec);
    undoable('Edit text', () => setState((s) => ({
      objects: s.objects.map((o) => {
        if (o.id !== object.id) return o;
        if (volume) return { ...o, volumes: o.volumes?.map((v) => (v.id === volume.id ? { ...v, geometry: centred, text: spec, name: nameOf(spec) } : v)) };
        return { ...o, geometry, source: file, text: spec, name: nameOf(spec), paint: undefined };
      }),
    })));
  });
}
