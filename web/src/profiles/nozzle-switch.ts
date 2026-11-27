import type { Config } from '../api/types';
import { sameValue } from '../state/value-equality';
import { getState, toast, type State } from '../state/store';
import { commitRecords } from './transactions';
import type { ResolveResponse } from '../flow/resolved';
import { catalogLine, defaultVariant, firmwareOf } from '../catalog/read';
import type { CatalogPrinter } from '../catalog/types';
import type { PrinterRecord } from './records';

const PRINTER_OWNED = /_gcode$|^printable_|^bed_|^curr_bed_type$|host|^thumbnails|^printer_notes$/;
const nozzleDependent = (key: string, from: Config, to: Config) => !PRINTER_OWNED.test(key) && (key in from || key in to) && !sameValue(key, from[key], to[key]);

const printerOf = (id: string) => getState().records.printers.find((p) => p.id === id && !p.archived);
const label = (key: string) => key.replace(/_/g, ' ');
const validSize = (diameter: number) => diameter >= 0.1 && diameter <= 2;
const knownVariant = (line: CatalogPrinter, id: string) => line.variants.some((v) => v.id === id);

export function nozzlePresets(printer: PrinterRecord, line: CatalogPrinter) {
  return (printer.nozzlePresets ?? []).filter((p) => p.variants.length === printer.nozzles.length &&
    p.variants.every((id) => knownVariant(line, id)));
}

export async function switchNozzle(printerId: string, size: number): Promise<string[]> {
  const before = printerOf(printerId);
  if (!before?.nozzles[0]) throw new Error('Oops! That printer isn\'t available right now.');
  if (!validSize(size)) throw new Error('Nozzles have to be between 0.1 and 2 mm!');
  const line = await catalogLine('printers', before.model);
  const current = line.variants.find((v) => v.id === before.nozzles[0].variant), firmware = current && firmwareOf(line, current);
  const catalog = defaultVariant(line, size, firmware);
  const saved = nozzlePresets(before, line).find((p) => p.diameter === size);
  const nozzles = before.nozzles.map((n, i) => ({ ...n, variant: catalog?.id ?? saved?.variants[i] ?? n.variant, diameter: size }));
  if (!nozzles.every((n) => knownVariant(line, n.variant))) throw new Error('Pick a catalog nozzle size before setting a custom size.');
  if (nozzles.every((n, i) => n.variant === before.nozzles[i].variant && n.diameter === before.nozzles[i].diameter)) return [];

  const presets = new Map((before.nozzlePresets ?? []).filter((p) => validSize(p.diameter) && p.variants.length === nozzles.length && p.variants.every(Boolean)).map((p) => [p.diameter, p]));
  for (const active of [before.nozzles, nozzles]) {
    const diameter = active[0].diameter;
    if (validSize(diameter) && active.every((n) => n.diameter === diameter && knownVariant(line, n.variant)) &&
      !defaultVariant(line, diameter, firmware))
      presets.set(diameter, { diameter, variants: active.map((n) => n.variant) });
  }
  const bases = nozzles.flatMap((n, i) => {
    const was = before.nozzles[i].variant;
    if (was === n.variant) return [];
    const from = line.variants.find((v) => v.id === was), to = line.variants.find((v) => v.id === n.variant);
    return from && to ? [[from.values, to.values] as const] : [];
  });
  const changed = (key: string) => bases.some(([from, to]) => nozzleDependent(key, from, to));
  const dropped: string[] = [], machine: Config = {};
  for (const [k, v] of Object.entries(before.machine ?? {})) if (changed(k)) dropped.push(k); else machine[k] = v;
  const next: PrinterRecord = { ...before, nozzles, nozzlePresets: presets.size ? [...presets.values()] : undefined, ...(bases.length ? { machine } : {}) };
  await sliceable(next);
  const document = await commitRecords([{ kind: 'printer', record: next, base: before }], { label: 'Nozzle', apply: bases.length ? (s) => follow(s, printerId, changed, dropped) : undefined });
  const savedRevision = document.printers.find((p) => p.id === printerId)?.revision;
  const undo = savedRevision === undefined ? undefined : { action: { label: 'Undo', run: () => void restore(before, next, savedRevision) } };
  if (dropped.length) toast('nozzle_switched_dropped', { name: before.name, size, dropped: dropped.map(label).join(', ') }, undo);
  else toast('nozzle_switched', { name: before.name, size }, undo);
  return dropped;
}

async function sliceable(next: PrinterRecord) {
  const s = getState();
  if (!s.api || !s.process) return;
  const slots = next.slots.filter((slot) => slot.filament);
  const request = { printer: next.id, print: s.process, filaments: slots.map((slot) => slot.filament), usedSlots: slots.map((_, i) => i + 1), layerHeight: null, project: {} };
  const reason = async (drafts?: object) => {
    const r = await fetch('/api/resolve', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ ...request, ...(drafts ? { drafts } : {}) }) });
    if (!r.ok) return null;
    const answer = await r.json() as ResolveResponse, status = [answer.printer.status, answer.print.status].find((x) => !x.ok);
    return status && !status.ok && status.code === 'unresolved' ? status.message : null;
  };
  const [now, proposed] = await Promise.all([reason(), reason({ nozzles: { [next.id]: next.nozzles } })]);
  if (proposed && proposed !== now) throw new Error(proposed);
}

function follow(s: State, printerId: string, changed: (key: string) => boolean, dropped: string[]): Partial<State> {
  if (s.printer !== printerId) return {};
  const strip = (config: Config) => Object.fromEntries(Object.entries(config).filter(([k]) => !changed(k) || (dropped.push(k), false)));
  return { dirty: { ...s.dirty, machine: strip(s.dirty.machine) }, projectPresets: s.projectPresets && { ...s.projectPresets, machine: strip(s.projectPresets.machine) } };
}

async function restore(previous: PrinterRecord, switched: PrinterRecord, revision: number) {
  const current = printerOf(previous.id);
  if (!current) { toast('cant_undo_because_printer'); return; }
  if (current.revision !== revision) { toast('nozzle_switch_failed_2', { detail: 'Can’t undo: this printer changed after the nozzle was switched.' }); return; }
  try { await commitRecords([{ kind: 'printer', record: previous, base: switched }], { label: 'Nozzle' }); toast('nozzle_restored', { name: previous.name }); }
  catch (e) { toast('nozzle_switch_failed_2', { detail: e instanceof Error ? e.message : String(e) }); }
}
