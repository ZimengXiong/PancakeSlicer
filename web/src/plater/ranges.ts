import { objectValues } from '../profiles/view';
import type { Config, ConfigValue } from '../api/types';
import { getState, markStale, newId, setState, type LayerRange, type PlateObject } from '../state/store';
import { undoable } from '../state/history';

const mapRanges = (objectId: number, fn: (rs: LayerRange[]) => LayerRange[], name: string, merge?: string) =>
  undoable(name, () => setState((s) => ({
    objects: s.objects.map((o): PlateObject => (o.id === objectId ? { ...o, ranges: fn(o.ranges ?? []) } : o)),
    job: markStale(s.job),
  })), merge);

const defaultHeight = (o: PlateObject) => Number(objectValues(getState(), o.id).values.layer_height) || 0.2;

export function addHeightRanges(objectId: number) {
  const o = getState().objects.find((x) => x.id === objectId);
  if (!o) return;
  let range = o.ranges?.[0];
  if (!range) {
    range = { id: newId(), z: [0, 2], layer_height: defaultHeight(o), settings: {} };
    const first = range;
    mapRanges(objectId, () => [first], 'Add layers');
  }
  setState({ selected: { object: o.id, instance: o.instances[0].id, allInstances: true, range: range.id } });
}

export function addRangeAfter(objectId: number, rangeId: number) {
  const o = getState().objects.find((x) => x.id === objectId), rs = [...(o?.ranges ?? [])].sort((a, b) => a.z[0] - b.z[0]);
  const k = rs.findIndex((r) => r.id === rangeId);
  if (!o || k < 0) return;
  const cur = rs[k], next = rs[k + 1];
  const z: [number, number] = next ? [cur.z[1], next.z[0]] : [cur.z[1], cur.z[1] + 2];
  if (z[1] - z[0] < 0.01) return;
  const added = { id: newId(), z, layer_height: defaultHeight(o), settings: {} };
  mapRanges(objectId, (list) => [...list, added].sort((a, b) => a.z[0] - b.z[0]), 'Add Height Range');
  setState((s) => ({ selected: s.selected && { ...s.selected, range: added.id } }));
}

export function canAddAfter(o: PlateObject, rangeId: number) {
  const rs = [...(o.ranges ?? [])].sort((a, b) => a.z[0] - b.z[0]), k = rs.findIndex((r) => r.id === rangeId);
  return k >= 0 && (k === rs.length - 1 || rs[k + 1].z[0] - rs[k].z[1] >= 0.01);
}

export function editRange(objectId: number, rangeId: number, patch: Partial<Pick<LayerRange, 'z' | 'layer_height'>>): boolean {
  const o = getState().objects.find((x) => x.id === objectId), r = o?.ranges?.find((x) => x.id === rangeId);
  if (!o || !r) return false;
  const z = patch.z ?? r.z;
  if (!(z[0] >= 0 && z[1] > z[0]) || o.ranges!.some((x) => x.id !== rangeId && x.z[0] < z[1] && z[0] < x.z[1])) return false;
  if (patch.layer_height !== undefined && !(patch.layer_height > 0)) return false;
  mapRanges(objectId, (list) => list.map((x) => (x.id === rangeId ? { ...x, ...patch, z } : x)).sort((a, b) => a.z[0] - b.z[0]), 'Edit Height Range', `range:${rangeId}`);
  return true;
}

export function deleteRange(objectId: number, rangeId: number) {
  mapRanges(objectId, (list) => list.filter((x) => x.id !== rangeId), 'Delete Height Range');
  setState((s) => ({ selected: s.selected?.range === rangeId ? { ...s.selected, range: undefined } : s.selected }));
}

export function setRangeOption(objectId: number, rangeId: number, key: string, value: ConfigValue) {
  mapRanges(objectId, (list) => list.map((x) => (x.id === rangeId ? { ...x, settings: { ...x.settings, [key]: value } } : x)), 'Change Height Range Settings', `range:${rangeId}:${key}`);
}

export function removeRangeOptions(objectId: number, rangeId: number, keys: string[]) {
  mapRanges(objectId, (list) => list.map((x) => {
    if (x.id !== rangeId) return x;
    const settings: Config = { ...x.settings };
    for (const k of keys) delete settings[k];
    return { ...x, settings };
  }), 'Delete Settings');
}

export const rangeLabel = (r: LayerRange) => `Range ${r.z[0].toFixed(2)}-${r.z[1].toFixed(2)} mm`;
