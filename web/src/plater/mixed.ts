import { effectiveConfig, getState, markStale, setState, type Mix, type PlateObject, type State } from '../state/store';
import { undoable } from '../state/history';
import type { Config } from '../api/types';
import { filamentColor } from './filamentColor';
import { remapObject } from '../components/RemapFilaments';
import { blend } from './mixColour';
import type { PaintNode, Painting } from './paint';
import { insertFlushSlots, retainFlushSlots } from '../multinozzle/flush-mapping';
import type { FlushingRecord } from '../profiles/records';
import { slotFilaments, slotMixes, stripOf, type Strip } from '../profiles/slots';
import { slotExtruder } from '../printers/feeds';
import { writeStrip } from '../profiles/slot-writes';

export const SLOTS_MAX = 32;
export { blend };

export const physicalCount = (s: State) => slotFilaments(s).length - Object.keys(slotMixes(s)).length;
export const componentColour = (s: State, c: number) => (c >= 1 && c <= physicalCount(s) ? filamentColor(s, c) : '#808080');
export const mixColour = (s: State, m: Mix) => blend(m.components.map((c) => componentColour(s, c)), m.ratios);
export const filamentType = (s: State, c: number) => String([effectiveConfig(s, 'filament', c - 1).filament_type].flat()[0] ?? '');

export function broken(s: State, m: Mix) {
  const n = physicalCount(s);
  const printer = s.records.printers.find((p) => p.id === s.printer), count = printer?.nozzles.length ?? 1;
  return m.components.length < 2 || m.components.some((c) => c < 1 || c > n) || new Set(m.components.map((c) => filamentType(s, c))).size > 1
    || count > 1 && new Set(m.components.map((c) => slotExtruder(printer, count, c - 1))).size > 1;
}

export function remapAll(st: State, map: number[]): Partial<State> {
  const each = (os: PlateObject[]) => os.map((o) => remapObject(o, map));
  const events = (es: State['events']) => es.flatMap((e) => (e.type !== 'change' ? [e] : map[Number(e.text) - 1] ? [{ ...e, text: String(map[Number(e.text) - 1]) }] : []));
  return {
    objects: each(st.objects), events: events(st.events),
    projectPlates: st.projectPlates.map((p) => ({ ...p,
      ...(p.index === st.plateIndex ? {} : { objects: each(p.objects), events: p.events && events(p.events) }) })),
    job: markStale(st.job),
  };
}

function retainedFlushing(st: State, kept: number[]): FlushingRecord | undefined {
  const flushing = st.records.printers.find((p) => p.id === st.printer)?.flushing, volumes = retainFlushSlots(flushing?.volumes, slotFilaments(st).length, kept);
  return flushing && volumes ? { ...flushing, volumes } : undefined;
}

const shifted = (mixes: Record<number, Mix>, from: number, by: number) =>
  Object.fromEntries(Object.entries(mixes).map(([i, m]) => [Number(i) >= from ? Number(i) + by : Number(i), m]));

export function addMix(m: Mix) {
  undoable('Add mixed filament', () => {
    const st = getState(), strip = stripOf(st), i = strip.filaments.length;
    const next: Strip = { ids: [...strip.ids, undefined], filaments: [...strip.filaments, strip.filaments[m.components[0] - 1] ?? strip.filaments[0]], colours: [...strip.colours, undefined], mixes: { ...strip.mixes, [i]: m } };
    setState({ job: markStale(st.job) });
    writeStrip(next, 'Add mixed filament', st, insertedFlush(st, [...strip.filaments.map((_, k) => k), -1]));
  });
}

export function editMix(i: number, m: Mix) {
  undoable('Edit mixed filament', () => {
    const st = getState(), strip = stripOf(st);
    setState({ job: markStale(st.job) });
    writeStrip({ ...strip, mixes: { ...strip.mixes, [i]: m } }, 'Edit mixed filament');
  });
}

export function deleteMixes(slots: number[], into?: number, name = 'Delete mixed filament') {
  undoable(name, () => {
    const st = getState(), strip = stripOf(st);
    const gone = new Set(slots), n = strip.filaments.length;
    const kept = Array.from({ length: n }, (_, i) => i).filter((i) => !gone.has(i));
    const id = (k: number) => kept.indexOf(k) + 1;
    const map = Array.from({ length: n }, (_, k) => (gone.has(k) ? (into ? id(into - 1) : 1) : id(k)));
    setState(remapAll(st, map));
    writeStrip({ ids: kept.map((k) => strip.ids[k]), filaments: kept.map((k) => strip.filaments[k]), colours: kept.map((k) => strip.colours[k]),
      mixes: Object.fromEntries(kept.flatMap((k, j) => (strip.mixes[k] ? [[j, strip.mixes[k]]] : []))) }, name, st, retainedFlushing(st, kept));
  });
}

const paints = (p: Painting | undefined, n: number): boolean => {
  const walk = (x: PaintNode): boolean => (Array.isArray(x) ? x.some(walk) : x === n);
  return !!p && [...p.values()].some(walk);
};
const usesSlot = (o: PlateObject, n: number) => o.extruder === n || paints(o.paint?.color, n)
  || !!o.volumes?.some((v) => v.type === 'part' && (v.extruder === n || paints(v.paint?.color, n)));

export function removeSlotAt(n: number): { moved: number; to: number; merged: { mix: number; into: number }[] } | null {
  const st0 = getState(), total = slotFilaments(st0).length, p = physicalCount(st0);
  if (n < 1 || n > total || total < 2 || (n <= p && p < 2)) return null;
  const target = n > 1 ? n - 1 : 2;
  const renumber = (j: number) => (j > n ? j - 1 : j);
  const to = renumber(target);
  const physical = (c: number) => (c === n ? to : c >= 1 && c <= total ? renumber(c) : c);
  const remixed = Object.entries(slotMixes(st0)).flatMap(([k, m]) => (Number(k) === n - 1 ? [] : [[Number(k), { ...m, components: m.components.map(physical) }] as const]));
  const merged = remixed.filter(([, m]) => new Set(m.components).size === 1).map(([k, m]) => ({ mix: k + 1, into: m.components[0] }));
  const gone = new Set([n - 1, ...merged.map((d) => d.mix - 1)]);
  const kept = Array.from({ length: total }, (_, k) => k).filter((k) => !gone.has(k));
  const map = Array.from({ length: total }, (_, k) => (k === n - 1 ? to : merged.find((d) => d.mix === k + 1)?.into ?? kept.indexOf(k) + 1));
  const moved = st0.objects.filter((o) => usesSlot(o, n)).length;
  undoable('Remove filament', () => {
    const st = getState(), strip = stripOf(st);
    const pick = <T,>(a: T[]) => kept.map((k) => Array.from(a)[k]);
    setState({ ...remapAll(st, map), projectPresets: st.projectPresets && { ...st.projectPresets, filaments: pick(st.projectPresets.filaments) } });
    writeStrip({ ids: pick(strip.ids), filaments: pick(strip.filaments), colours: pick(strip.colours),
      mixes: Object.fromEntries(remixed.filter(([k]) => !gone.has(k)).map(([k, m]) => [kept.indexOf(k), m])) }, 'Remove filament', st, retainedFlushing(st, kept));
  });
  return { moved, to, merged };
}

function insertedFlush(st: State, previous: number[]): FlushingRecord | undefined {
  const flushing = st.records.printers.find((p) => p.id === st.printer)?.flushing, matrix = flushing?.volumes;
  const n = slotFilaments(st).length;
  if (!flushing || !matrix?.length || !n || matrix.length % (n * n)) return undefined;
  return { ...flushing, volumes: insertFlushSlots(matrix, n, previous) };
}

export function addPhysical(colour?: string, name?: string) {
  undoable('Add filament', () => {
    const st = getState(), strip = stripOf(st), p = physicalCount(st), n = strip.filaments.length;
    const ids = [...strip.ids], filaments = [...strip.filaments], colours = [...strip.colours];
    ids.splice(p, 0, undefined);
    filaments.splice(p, 0, name ?? strip.filaments[p - 1] ?? '');
    colours.splice(p, 0, colour);
    const next: Strip = { ids, filaments, colours, mixes: shifted(strip.mixes, p, 1) };
    const previous = Array.from({ length: n + 1 }, (_, k) => k === p ? -1 : k > p ? k - 1 : k);
    if (writeStrip(next, 'Add filament', st, insertedFlush(st, previous))) setState(remapAll(st, Array.from({ length: n }, (_, k) => (k >= p ? k + 2 : k + 1))));
  });
}

export function removePhysical() {
  undoable('Remove filament', () => {
    const st = getState(), strip = stripOf(st), p = physicalCount(st), n = strip.filaments.length;
    if (p <= 1) return;
    const ids = [...strip.ids], filaments = [...strip.filaments], colours = [...strip.colours];
    ids.splice(p - 1, 1);
    filaments.splice(p - 1, 1);
    colours.splice(p - 1, 1);
    const mixes = Object.fromEntries(Object.entries(shifted(strip.mixes, p, -1)).map(([i, m]) =>
      [i, { ...m, components: m.components.map((c) => (c === p ? 0 : c > p ? c - 1 : c)) }]));
    const kept = Array.from({ length: n }, (_, k) => k).filter((k) => k !== p - 1);
    setState(remapAll(st, Array.from({ length: n }, (_, k) => (k === p - 1 ? 1 : k >= p ? k : k + 1))));
    writeStrip({ ids, filaments, colours, mixes }, 'Remove filament', st, retainedFlushing(st, kept));
  });
}

export function recommendations(s: State, three: boolean): Mix[] {
  const byType = new Map<string, number[]>();
  for (let c = 1; c <= physicalCount(s); c++) {
    const t = filamentType(s, c);
    if (!t.endsWith('-S')) byType.set(t, [...(byType.get(t) ?? []), c]);
  }
  const out: Mix[] = [];
  for (const ids of byType.values())
    for (let a = 0; a < ids.length; a++)
      for (let b = a + 1; b < ids.length; b++) {
        if (!three) out.push({ components: [ids[a], ids[b]], ratios: [50, 50], gradient: null, perPart: false });
        else for (let c = b + 1; c < ids.length; c++)
          for (let k = 0; k < 3; k++) out.push({ components: [ids[a], ids[b], ids[c]], ratios: [0, 1, 2].map((j) => (j === k ? 50 : 25)), gradient: null, perPart: false });
      }
  return out.slice(0, 100);
}

export function readMixes(c: Config): Record<number, Mix> {
  const at = (k: string, i: number) => { const v = c[k]; return String(Array.isArray(v) ? v[i] ?? '' : ''); };
  const on = (k: string, i: number) => ['1', 'true'].includes(at(k, i));
  const mixes: Record<number, Mix> = {};
  const n = Array.isArray(c.filament_is_mixed) ? c.filament_is_mixed.length : 0;
  for (let i = 0; i < n; i++) if (on('filament_is_mixed', i)) {
    const components = at('filament_mixed_components', i).split(',').filter(Boolean).map(Number);
    let ratios = at('filament_mixed_sublayer_ratios', i).split(',').filter(Boolean).map((r) => Math.trunc(Number(r) * 100 + 0.5));
    if (ratios.length !== components.length) ratios = components.map((_, k) => Math.trunc(100 / components.length) + (k ? 0 : 100 % components.length));
    const gradient = on('filament_mixed_gradient', i) ? (at('filament_mixed_gradient_range', i).startsWith('0.1') ? 1 : 0) : null;
    mixes[i] = { components, ratios, gradient, perPart: on('filament_mixed_gradient_per_part', i) };
  }
  return mixes;
}
