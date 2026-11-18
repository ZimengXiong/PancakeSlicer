import { effectiveConfig, getState, markStale, setState, type PlateObject, type State } from '../state/store';
import { undoable } from '../state/history';
import { slotFilaments, stripOf, withStrip } from '../profiles/slots';
import { paintOf, type PaintNode } from '../plater/paint';
import type { Config } from '../api/types';

export interface SlotIssues { missing: Map<number, string[]>; empty: Map<number, string[]> }

const used = (o: PlateObject) => [o.extruder || 1, ...(o.volumes ?? []).map((v) => v.extruder).filter((n) => n > 0)];

const leaves = (node: PaintNode): number[] => typeof node === 'number' ? [node] : node.flatMap(leaves);
export function paintedSlots(o: PlateObject): number[] {
  const out = new Set<number>();
  for (const p of [paintOf(o, 'color'), ...(o.volumes ?? []).filter((v) => v.type === 'part').map((v) => paintOf(v, 'color'))])
    for (const node of p?.values() ?? []) for (const n of leaves(node)) if (n > 0) out.add(n);
  out.delete(o.extruder || 1);
  return [...out].sort((a, b) => a - b);
}

const FILAMENT_KEYS = ['extruder', 'wall_filament', 'sparse_infill_filament', 'solid_infill_filament', 'support_filament', 'support_interface_filament'] as const;
export function plateSlots(s: State): number[] {
  const slots = slotFilaments(s).length, out = new Set<number>();
  const settings = (c: Config) => { for (const key of FILAMENT_KEYS) { const n = Number(c[key]); if (n > 0) out.add(n); } };
  let all = false;
  for (const o of s.objects) {
    const object = o.extruder || 1;
    out.add(object); settings(o.settings); all ||= !!paintOf(o, 'color');
    for (const v of o.volumes ?? []) { out.add(v.extruder || object); settings(v.settings); all ||= v.type === 'part' && !!paintOf(v, 'color'); }
    for (const r of o.ranges ?? []) { out.add(object); settings(r.settings); }
  }
  return Array.from({ length: slots }, (_, i) => i + 1).filter((n) => all || out.has(n));
}

export function slotIssues(s: State): SlotIssues {
  const missing = new Map<number, string[]>(), empty = new Map<number, string[]>();
  for (const o of s.objects) for (const n of new Set(used(o))) {
    const into = n > slotFilaments(s).length ? missing : !slotFilaments(s)[n - 1] ? empty : null;
    if (into) into.set(n, [...(into.get(n) ?? []), o.name]);
  }
  return { missing, empty };
}

const list = (m: Map<number, string[]>, what: (n: number) => string) => [...m].map(([n, objects]) => `${objects.join(', ')}: ${what(n)}`).join('\n');
export const missingText = (i: SlotIssues) => list(i.missing, (n) => `slot ${n}, which this printer doesn't have`);
export const emptyText = (i: SlotIssues) => list(i.empty, (n) => `slot ${n}, which has no filament chosen`);

export function remapMissing(issues: SlotIssues) {
  const s = getState(), taken = new Set(s.objects.flatMap(used));
  const free = slotFilaments(s).map((_, i) => i + 1).filter((n) => slotFilaments(s)[n - 1] && !taken.has(n));
  const target = new Map([...issues.missing.keys()].map((n, i) => [n, free[i] ?? (slotFilaments(s).findIndex(Boolean) + 1 || 1)]));
  const move = (n: number) => target.get(n) ?? n;
  const fix = (o: PlateObject): PlateObject => ({ ...o, extruder: move(o.extruder || 1), ...(o.volumes ? { volumes: o.volumes.map((v) => v.extruder > 0 ? { ...v, extruder: move(v.extruder) } : v) } : {}) });
  undoable('Change Filament', () => setState((st) => ({ objects: st.objects.map(fix), job: markStale(st.job) })));
}

export function slicedSlots(s: State): State {
  const print = effectiveConfig(s, 'process');
  const top = Math.max(1, ...s.objects.flatMap(used), ...plateSlots(s), ...FILAMENT_KEYS.map((key) => Number(print[key]) || 0),
    ...s.events.flatMap((e) => e.type === 'change' ? [Number(e.text) || 0] : []));
  const strip = stripOf(s);
  return withStrip(s, { ids: strip.ids.slice(0, top), filaments: strip.filaments.slice(0, top), colours: strip.colours.slice(0, top),
    mixes: Object.fromEntries(Object.entries(strip.mixes).filter(([k]) => Number(k) < top)) });
}
