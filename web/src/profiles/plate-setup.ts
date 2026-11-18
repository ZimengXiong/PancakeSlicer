import type { FlushingRecord, PrinterRecord } from './records';
import type { PlateSetup } from '../flow/resolved';
import { getState, toast, type State } from '../state/store';
const commitRecords = (...args: Parameters<typeof import('./transactions').commitRecords>) => import('./transactions').then((m) => m.commitRecords(...args));

export function plateSetup(s: State, printer = s.printer): PlateSetup | null {
  const r = s.resolved;
  if (printer === s.printer) return r.answer?.setup && r.answeredFor?.printer === s.printer ? r.answer.setup : null;
  return r.answer?.profiles?.find((p) => p.type === 'machine' && p.id === printer)?.setup ?? null;
}

export const plateLabel = (setup: PlateSetup | null, value: string | undefined) => setup?.plates.find((p) => p.value === value)?.label ?? value ?? '';

function writePrinter(edit: (record: PrinterRecord) => PrinterRecord, label: string, printer = getState().printer): Promise<unknown> {
  const record = getState().records.printers.find((p) => p.id === printer);
  if (!record) return Promise.reject(new Error('Pick a printer first!'));
  const next = edit(record);
  return JSON.stringify(next) === JSON.stringify(record) ? Promise.resolve()
    : commitRecords([{ kind: 'printer', record: next }], { label, optimistic: true });
}

export function setBuildPlate(plate: string, printer?: string): Promise<unknown> {
  return writePrinter((r) => ({ ...r, plate }), 'Change build plate', printer).catch((e) => { toast('plate_setup_failed', { detail: e instanceof Error ? e.message : String(e) }); });
}

export function setFlushing(flushing: FlushingRecord | undefined): Promise<unknown> {
  return writePrinter((r) => {
    const { flushing: _old, ...rest } = r;
    return flushing && (flushing.volumes?.length || flushing.multipliers?.length) ? { ...rest, flushing } : rest;
  }, 'Flushing volumes').catch((e) => { toast('plate_setup_failed_2', { detail: e instanceof Error ? e.message : String(e) }); });
}
