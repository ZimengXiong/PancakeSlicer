import type { Mix, State } from '../state/store';
import { stripOf, type Strip } from '../profiles/slots';
import { feedOf, slotPositions, unitsOf } from '../printers/feeds';
import { remapAll } from '../plater/mixed';
import { profileInfo } from '../profiles/view';

export function placeProject(s: State, planned: Strip, filaments: string[], placed: readonly boolean[]): { strip: Strip; map: number[]; unplaced: string[]; replaced: string[] } {
  const own = stripOf(s), record = s.records.printers.find((p) => p.id === s.printer);
  const physical = own.filaments.flatMap((_, i) => i in own.mixes ? [] : [i]);
  const ownMixes = Object.keys(own.mixes).map(Number).sort((a, b) => a - b);
  const feed = feedOf(record?.model), positions = feed && record ? slotPositions(feed, unitsOf(feed, record)) : [];
  const rank = (i: number) => { const at = positions.findIndex((p) => p.tray === record?.slots[i]?.tray); return at < 0 ? positions.length + i : at; };
  const ids = physical.map((i) => own.ids[i]), held = physical.map((i) => own.filaments[i]), colours = physical.map((i) => own.colours[i]);
  const name = (f: string) => profileInfo(s, 'filament', f).name || f;
  const map: number[] = [], unplaced: string[] = [], replaced: string[] = [], pending: number[] = [];
  const taken = new Set<number>();
  const project = filaments.flatMap((_, j) => placed[j] && !(j in planned.mixes) ? [j] : []);
  for (const [j, f] of filaments.entries()) {
    if (j in planned.mixes) continue;
    if (!project.includes(j)) { map[j] = j + 1; continue; }
    const at = f ? held.indexOf(f) : -1;
    if (at >= 0) { map[j] = at + 1; taken.add(at); } else pending.push(j);
  }
  for (const j of pending) {
    const f = filaments[j];
    const free = f ? held.map((_, k) => k).filter((k) => !held[k]).sort((a, b) => rank(a) - rank(b))[0] : undefined;
    const over = f && free === undefined ? held.findIndex((_, k) => !taken.has(k)) : -1;
    const grow = f && free === undefined && over < 0 && !feed && !ownMixes.length && held.length < project.length;
    if (free !== undefined) {
      held[free] = f; colours[free] = planned.colours[j] ?? colours[free]; map[j] = free + 1; taken.add(free);
    } else if (over >= 0) {
      replaced.push(`Slot ${over + 1}: ${name(held[over])} → ${name(f)}`);
      held[over] = f; colours[over] = planned.colours[j] ?? colours[over]; map[j] = over + 1; taken.add(over);
    } else if (grow) {
      ids.push(undefined); held.push(f); colours.push(planned.colours[j]); map[j] = held.length; taken.add(held.length - 1);
    } else if (f) unplaced.push(name(f));
  }
  const mixes: Record<number, Mix> = {};
  const out: Strip = { ids: [...ids], filaments: [...held], colours: [...colours], mixes };
  for (const k of ownMixes) { mixes[out.filaments.length] = own.mixes[k]; out.ids.push(own.ids[k]); out.filaments.push(own.filaments[k]); out.colours.push(undefined); }
  for (const j of Object.keys(planned.mixes).map(Number).sort((a, b) => a - b)) {
    const m = planned.mixes[j], components = m.components.map((c) => map[c - 1]);
    if (components.some((c) => !c || c > held.length)) { unplaced.push(`mixed filament ${j + 1}`); continue; }
    map[j] = out.filaments.length + 1;
    mixes[out.filaments.length] = { ...m, components };
    out.ids.push(undefined); out.filaments.push(held[components[0] - 1]); out.colours.push(undefined);
  }
  let past = out.filaments.length;
  for (let j = 0; j < filaments.length; j++) if (!map[j]) map[j] = ++past;
  return { strip: out, map, unplaced, replaced };
}

export function followPlacement(st: State, map: number[]): Partial<State> {
  const moved = map[0] !== 1;
  const objects = (os: State['objects']) => moved ? os.map((o) => (o.extruder ? o : { ...o, extruder: 1 })) : os;
  return remapAll({ ...st, objects: objects(st.objects), projectPlates: st.projectPlates.map((p) => ({ ...p, objects: objects(p.objects) })) }, map);
}

export const printerName = (s: State) => profileInfo(s, 'machine', s.printer).name || 'this printer';
