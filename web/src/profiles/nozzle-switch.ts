import type { Config } from '../api/types';
import { sameValue } from '../state/value-equality';
import { getState, toast, type State } from '../state/store';
import { commitRecords } from './transactions';
import type { ResolveResponse } from '../flow/resolved';
import { catalogLine, defaultVariant } from '../catalog/read';
import type { CatalogVariant } from '../catalog/types';
import type { PrinterRecord } from './records';

const PRINTER_OWNED = /_gcode$|^printable_|^bed_|^curr_bed_type$|host|^thumbnails|^printer_notes$/;
const nozzleDependent = (key: string, from: Config, to: Config) => !PRINTER_OWNED.test(key) && (key in from || key in to) && !sameValue(key, from[key], to[key]);

const printerOf = (id: string) => getState().records.printers.find((p) => p.id === id && !p.archived);
const label = (key: string) => key.replace(/_/g, ' ');

export async function switchNozzle(printerId: string, size: number): Promise<string[]> {
  const before = printerOf(printerId);
  if (!before?.nozzles[0]) throw new Error('Oops! That printer isn\'t available right now.');
  const line = await catalogLine('printers', before.model), to = defaultVariant(line, size), from = line.variants.find((v) => v.id === before.nozzles[0].variant);
  if (!to) throw new Error(`Hmm, ${before.model} doesn't have a ${size} mm nozzle in the catalog.`);
  if (before.nozzles.every((n) => n.variant === to.id && n.diameter === size)) return [];
  const dropped: string[] = [], machine: Config = {};
  for (const [k, v] of Object.entries(before.machine ?? {})) if (from && nozzleDependent(k, from.values, to.values)) dropped.push(k); else machine[k] = v;
  const next: PrinterRecord = { ...before, nozzles: before.nozzles.map((n) => ({ ...n, variant: to.id, diameter: size })), machine };
  await sliceable(next);
  await commitRecords([{ kind: 'printer', record: next }], { label: 'Nozzle', apply: (s) => follow(s, printerId, from, to, dropped) });
  const undo = { action: { label: 'Undo', run: () => void restore(before) } };
  if (dropped.length) toast('nozzle_switched_dropped', { name: before.name, size, dropped: dropped.map(label).join(', ') }, undo);
  else toast('nozzle_switched', { name: before.name, size }, undo);
  return dropped;
}

export async function setNozzleDiameter(printerId: string, diameter: number): Promise<void> {
  const before = printerOf(printerId);
  if (!before?.nozzles[0]) throw new Error('Oops! That printer isn\'t available right now.');
  if (!(diameter >= 0.1 && diameter <= 2)) throw new Error('Nozzles have to be between 0.1 and 2 mm!');
  const next = { ...before, nozzles: before.nozzles.map((n) => ({ ...n, diameter })) };
  await sliceable(next);
  await commitRecords([{ kind: 'printer', record: next }], { label: 'Nozzle' });
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

function follow(s: State, printerId: string, from: CatalogVariant | undefined, to: CatalogVariant, dropped: string[]): Partial<State> {
  if (s.printer !== printerId || !from) return {};
  const strip = (config: Config) => Object.fromEntries(Object.entries(config).filter(([k]) => !nozzleDependent(k, from.values, to.values) || (dropped.push(k), false)));
  return { dirty: { ...s.dirty, machine: strip(s.dirty.machine) }, projectPresets: s.projectPresets && { ...s.projectPresets, machine: strip(s.projectPresets.machine) } };
}

async function restore(previous: PrinterRecord) {
  const current = printerOf(previous.id);
  if (!current) { toast('cant_undo_because_printer'); return; }
  try { await commitRecords([{ kind: 'printer', record: { ...previous, revision: current.revision } }], { label: 'Nozzle' }); toast('nozzle_restored', { name: previous.name }); }
  catch (e) { toast('nozzle_switch_failed_2', { detail: e instanceof Error ? e.message : String(e) }); }
}
