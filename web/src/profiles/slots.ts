import type { Mix, State } from '../state/store';
import type { MixRecord, PrinterRecord, SlotRecord } from './records';
import { randomId } from '../random-id';
import { nextTray } from '../printers/feeds';

const NO_FILAMENTS: string[] = [];
const NO_MIXES: Record<number, Mix> = {};
const selected = (s: Pick<State, 'records' | 'printer'>) => s.printer ? s.records.printers.find((p) => p.id === s.printer && !p.archived) : undefined;

interface Derived { ids: (string | undefined)[]; filaments: string[]; colours: (string | undefined)[]; mixes: Record<number, Mix> }
const derived = new WeakMap<PrinterRecord, Derived>();
function derive(record: Pick<PrinterRecord, 'slots' | 'mixes'> & object): Derived {
  let out = derived.get(record as PrinterRecord);
  if (out) return out;
  const slots = record.slots.length ? record.slots : [{ id: undefined, filament: '' } as unknown as SlotRecord];
  const number = new Map(record.slots.map((slot, i) => [slot.id, i + 1]));
  const mixes: Record<number, Mix> = {};
  const ids: (string | undefined)[] = slots.map((slot) => slot.id), filaments = slots.map((slot) => slot.filament);
  for (const [k, m] of (record.mixes ?? []).entries()) {
    const components = m.components.map((id) => number.get(id) ?? 0);
    mixes[slots.length + k] = { components, ratios: m.ratios, gradient: m.gradient, perPart: m.perPart };
    ids.push(m.id);
    filaments.push(filaments[(components[0] ?? 1) - 1] ?? filaments[0]);
  }
  out = { ids, filaments, colours: [...slots.map((slot) => slot.colour), ...(record.mixes ?? []).map(() => undefined)], mixes };
  derived.set(record as PrinterRecord, out);
  return out;
}

export function slotFilaments(s: Pick<State, 'records' | 'printer'>): string[] {
  const record = selected(s);
  return record ? derive(record).filaments : NO_FILAMENTS;
}

export function slotIds(s: Pick<State, 'records' | 'printer'>): (string | undefined)[] {
  const record = selected(s);
  return record ? derive(record).ids : [];
}

export const slotMixes = (s: Pick<State, 'records' | 'printer'>): Record<number, Mix> => {
  const record = selected(s);
  return record ? derive(record).mixes : NO_MIXES;
};

export function slotAt(s: Pick<State, 'records'>, printer: string, i: number): SlotRecord | undefined {
  return s.records.printers.find((p) => p.id === printer)?.slots[i];
}

export function mixAt(s: Pick<State, 'records'>, printer: string, i: number): Mix | undefined {
  const record = s.records.printers.find((p) => p.id === printer);
  return record ? derive(record).mixes[i] : undefined;
}

export interface Strip { ids: (string | undefined)[]; filaments: string[]; colours: (string | undefined)[]; mixes: Record<number, Mix> }

export function stripOfRecord(record: PrinterRecord): Strip {
  const d = derive(record);
  return { ids: [...d.ids], filaments: [...d.filaments], colours: [...d.colours], mixes: d.mixes };
}

export function stripOf(s: Pick<State, 'records' | 'printer'>): Strip {
  const record = selected(s);
  return record ? stripOfRecord(record) : { ids: [], filaments: [], colours: [], mixes: NO_MIXES };
}

export function documentStrip(doc: { printer?: string; profileSnapshots?: State['profileSnapshots']; filamentSlots?: unknown; slotColours?: unknown; mixes?: unknown }): Strip | null {
  if (Array.isArray(doc.filamentSlots)) return { ids: doc.filamentSlots.map(() => undefined), filaments: doc.filamentSlots.map(String), colours: Array.isArray(doc.slotColours) ? [...doc.slotColours] : [],
    mixes: doc.mixes && typeof doc.mixes === 'object' ? doc.mixes as Record<number, Mix> : NO_MIXES };
  const printer = doc.profileSnapshots?.printer, slots = doc.profileSnapshots?.slots;
  return printer && printer.id === doc.printer ? stripOfRecord(slots ? { ...printer, slots } : printer) : null;
}

export function recordWithStrip(s: Pick<State, 'records' | 'printer'>, strip: Strip): PrinterRecord | undefined {
  const record = selected(s);
  if (!record) return undefined;
  const own = new Map(record.slots.map((slot) => [slot.id, slot]));
  const physical = strip.filaments.flatMap((_, i) => i in strip.mixes ? [] : [i]);
  const kept = physical.flatMap((i) => { const slot = strip.ids[i] && own.get(strip.ids[i]!); return slot ? [slot] : []; });
  const slots: SlotRecord[] = [];
  for (const i of physical) {
    const slot = strip.ids[i] ? own.get(strip.ids[i]!) : undefined;
    const tray = slot ? slot.tray : nextTray(record, [...kept, ...slots]);
    slots.push({ id: slot?.id ?? `slot:${randomId()}`, filament: strip.filaments[i] ?? '',
      ...(strip.colours[i] ? { colour: strip.colours[i] } : {}), ...(tray ? { tray } : {}) });
  }
  const mixes: MixRecord[] = Object.keys(strip.mixes).map(Number).sort((a, b) => a - b).map((k) => {
    const m = strip.mixes[k];
    return { id: strip.ids[k] ?? `mix:${randomId()}`, components: m.components.map((n) => slots[n - 1]?.id ?? ''), ratios: m.ratios, gradient: m.gradient, perPart: m.perPart };
  });
  const { mixes: _mixes, ...rest } = record;
  const next: PrinterRecord = { ...rest, slots, ...(mixes.length ? { mixes } : {}) };
  return JSON.stringify(next) === JSON.stringify(record) ? undefined : next;
}

export function withStrip<S extends Pick<State, 'records' | 'printer'>>(s: S, strip: Strip): S {
  const record = recordWithStrip(s, strip);
  return record ? { ...s, records: { ...s.records, printers: s.records.printers.map((p) => p.id === record.id ? record : p) } } : s;
}
