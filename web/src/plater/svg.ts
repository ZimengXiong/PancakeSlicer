import { beginBusy } from '../state/busy';
import * as THREE from 'three';
import { getState, newId, setState, toast, type PlateObject, type SvgSpec, type Volume, type VolumeType } from '../state/store';
import { undoable } from '../state/history';
import { bedArea } from '../actions';
import { bakeStl, bedBounds, nextFreeSpot } from './geometry';
import { meshJob } from './meshOps';
import { placeVolume } from './volumes';
import { inFrame, surfaceFields } from './text';

const NEW = { depth: 2, size: 30 };

const fields = (t: SvgSpec) => ({ depth: String(t.depth), ...(t.size ? { size: String(t.size) } : {}) });
const svgBlob = (t: SvgSpec) => new Blob([t.svg], { type: 'image/svg+xml' });
const warn = (record: Record<string, unknown>, name: string) => {
  const w = (record.warnings as string[] | undefined) ?? [];
  if (w.length || record.best_effort) toast('svg_failed', { name, value: w.length ? w.join('; ') : 'best effort result' });
};

function pick(): Promise<File | null> {
  return new Promise((resolve) => {
    const input = document.createElement('input');
    input.type = 'file';
    input.accept = '.svg,image/svg+xml';
    input.onchange = () => resolve(input.files?.[0] ?? null);
    input.click();
  });
}

let busy = false;
async function withService(work: () => Promise<void>) {
  if (busy) return;
  busy = true;
  const overlay = beginBusy('Embossing SVG');
  try { await work(); } catch (e) { toast('svg', { detail: e instanceof Error ? e.message : String(e) }); } finally {
    busy = false;
    overlay.end();
  }
}

export async function addSvg(objectId: number | null, type: VolumeType = 'part') {
  const file = await pick();
  if (!file) return;
  const spec: SvgSpec = { svg: await file.text(), name: file.name.replace(/\.svg$/i, ''), ...NEW };
  await withService(async () => {
    const { record, file: stl, geometry } = await meshJob('svg', fields(spec), [[svgBlob(spec), `${spec.name}.svg`]], spec.name);
    warn(record, spec.name);
    if (objectId !== null) {
      const { centred } = inFrame(geometry);
      const id = placeVolume(objectId, type, spec.name, centred,
        (b, size) => new THREE.Vector3((b.min.x + b.max.x) / 2, (b.min.y + b.max.y) / 2, type === 'part' ? b.max.z + size.z / 2 : b.max.z - size.z / 2),
        'Add SVG', { svg: spec });
      const s = getState(), o = s.objects.find((x) => x.id === objectId);
      if (id !== undefined && o) setState({ selected: { object: o.id, instance: s.selected?.instance ?? o.instances[0].id, volume: id }, gizmo: 'svg' });
      return;
    }
    const { center } = bedBounds(bedArea());
    const o: PlateObject = { id: newId(), name: spec.name, geometry, rotX: 0, rotY: 0, scale: [1, 1, 1], extruder: 1, settings: {}, source: stl, svg: spec,
      instances: [{ id: newId(), x: center.x, y: center.y, rotZ: 0 }] };
    const spot = nextFreeSpot(getState().objects, o, bedArea()) ?? center;
    o.instances[0] = { ...o.instances[0], x: spot.x, y: spot.y };
    undoable('Add SVG', () => setState((s) => ({ objects: [...s.objects, o], selected: { object: o.id, instance: o.instances[0].id }, gizmo: 'svg' })));
  });
}

export function editedSvg(s = getState()): { object: PlateObject; volume?: Volume; spec: SvgSpec } | null {
  const o = s.objects.find((x) => x.id === s.selected?.object);
  if (!o) return null;
  const v = s.selected?.volume !== undefined ? o.volumes?.find((x) => x.id === s.selected!.volume) : undefined;
  if (v?.svg) return { object: o, volume: v, spec: v.svg };
  return o.svg && !v ? { object: o, spec: o.svg } : null;
}

export async function updateSvg(spec: SvgSpec) {
  const target = editedSvg();
  if (!target) return;
  await withService(async () => {
    const { object, volume } = target;
    if (volume) {
      const files: [Blob, string][] = [[svgBlob(spec), `${spec.name}.svg`], ...(spec.surface ? [[await bakeStl(object, new THREE.Matrix4()), 'target.stl'] as [Blob, string]] : [])];
      const { record, geometry } = await meshJob('svg', { ...fields(spec), ...(spec.surface ? surfaceFields(spec.surface, volume.type) : {}) }, files, spec.name);
      warn(record, spec.name);
      const { centred, matrix } = inFrame(geometry);
      undoable('Edit SVG', () => setState((s) => ({
        objects: s.objects.map((o) => (o.id !== object.id ? o : { ...o, volumes: o.volumes?.map((v) => (v.id === volume.id ? { ...v, geometry: centred, ...(spec.surface ? { matrix } : {}), svg: spec } : v)) })),
      })));
      return;
    }
    const { record, file, geometry } = await meshJob('svg', fields(spec), [[svgBlob(spec), `${spec.name}.svg`]], spec.name);
    warn(record, spec.name);
    undoable('Edit SVG', () => setState((s) => ({ objects: s.objects.map((o) => (o.id === object.id ? { ...o, geometry, source: file, svg: spec, paint: undefined } : o)) })));
  });
}

export function placeOnSurface(at: [number, number, number], normal: [number, number, number]) {
  const t = editedSvg();
  if (t?.volume) void updateSvg({ ...t.spec, surface: { at, normal } });
}
