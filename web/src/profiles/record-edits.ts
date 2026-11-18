import { getState } from '../state/store';
import { getRecord, INSTALLED, recordKey, type NozzleRecord, type PrinterRecord } from './records';
import { commitRecords } from './transactions';

export const MIXED_SIZES = 'Different nozzle sizes on the two sides aren\'t supported just yet — set both nozzles to the same size for now!';

export const printerRecord = (id: string): PrinterRecord | undefined => getRecord(getState().records, recordKey('printer', id)) as PrinterRecord | undefined;

export function setPrinterNozzle(printerId: string, i: number, patch: Partial<NozzleRecord>): Promise<unknown> {
  const printer = printerRecord(printerId);
  if (!printer || printer.archived) return Promise.reject(new Error('Please select a saved printer first.'));
  if (!printer.nozzles[i]) return Promise.reject(new Error(`Hmm, ${printer.name} doesn't have a ${i + 1} nozzle.`));
  if (patch.diameter !== undefined && !(patch.diameter >= 0.1 && patch.diameter <= 2)) return Promise.reject(new Error('Nozzles have to be between 0.1 and 2 mm!'));
  const nozzles = printer.nozzles.map((n, at) => at === i ? { ...n, ...patch } : { ...n });
  if (new Set(nozzles.map((n) => n.diameter)).size > 1) return Promise.reject(new Error(MIXED_SIZES));
  for (const field of ['volume', 'type'] as const) if (patch[field] !== undefined) for (const n of nozzles) n[field] ??= INSTALLED[field];
  return commitRecords([{ kind: 'printer', record: { ...printer, nozzles } }], { label: 'Nozzle' });
}
