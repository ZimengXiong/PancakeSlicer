import { getState, type State } from '../state/store';
import { recordOf } from '../profiles/view';
import { sameRecord, type PrinterRecord } from '../profiles/records';
import { layoutSetup, type FeedUnit } from './feeds';
import { rememberSpool } from './slots';
import type { Tray } from './connections';

const laidOut = new WeakMap<PrinterRecord, PrinterRecord>();
export function setupOf(s: State, printer: string): PrinterRecord | undefined {
  const record = recordOf(s, 'machine', printer) as PrinterRecord | undefined;
  if (!record) return undefined;
  let v = laidOut.get(record);
  if (!v) laidOut.set(record, (v = layoutSetup(record)));
  return v;
}

export function editSetup(printer: string, change: (record: PrinterRecord) => PrinterRecord, units?: FeedUnit[]): Promise<void> {
  const before = setupOf(getState(), printer);
  if (!before) return Promise.reject(new Error('Hmm, that printer isn\'t in your library anymore.'));
  const next = layoutSetup(change(structuredClone(before)), units, true);
  if (sameRecord(next, before)) return Promise.resolve();
  return import('../profiles/transactions').then(({ commitRecords }) => commitRecords([{ kind: 'printer', record: next }], { label: 'Printer setup', optimistic: true })).then(() => undefined);
}

export function reportedUnits(trays: readonly Tray[], nozzles: number): FeedUnit[] {
  const ids = [...new Set(trays.filter((t) => t.kind !== 'external' && t.id !== 254 && t.id !== 255).map((t) => t.ams))].sort((a, b) => a - b);
  return Array.from({ length: ids.length ? ids[ids.length - 1] + 1 : 0 }, (_, u): FeedUnit => ({ nozzle: nozzles > 1 ? trays.find((t) => t.ams === u)?.extruder ?? 0 : 0 }));
}

export async function pickSlotFilament(printer: string, tray: string, filament: string, units?: FeedUnit[], spool?: boolean) {
  await editSetup(printer, (setup) => ({ ...setup, slots: setup.slots.map((slot) => slot.tray === tray ? { ...slot, filament } : slot) }), units);
  if (spool) await rememberSpool(printer, tray, filament || null);
}

export async function setUnits(printer: string, units: FeedUnit[], unit: string): Promise<void> {
  const s = getState(), stored = recordOf(s, 'machine', printer) as PrinterRecord | undefined, before = setupOf(s, printer);
  if (!stored || !before) throw new Error('Hmm, that printer isn\'t in your library anymore.');
  await editSetup(printer, (x) => x, units);
  const after = setupOf(getState(), printer);
  const gone = before.slots.flatMap((slot, i) => after?.slots.some((a) => a.id === slot.id) ? [] : [{ n: i + 1, filament: slot.filament }]);
  if (!gone.length) return;
  const numbers = gone.length > 1 && gone.at(-1)!.n - gone[0].n === gone.length - 1 ? `slots ${gone[0].n}–${gone.at(-1)!.n}` : `slot${gone.length > 1 ? 's' : ''} ${gone.map((g) => g.n).join(', ')}`;
  const names = gone.flatMap((g) => g.filament ? [(recordOf(getState(), 'filament', g.filament)?.name ?? 'a removed filament')] : []);
  const { toast } = await import('../state/store');
  toast('popped_off_xx_removed', { unit, numbers, value: names.length ? ` (${names.join(', ')})` : '' }, { action: {
    label: 'Undo',
    run: () => void import('../profiles/transactions').then(({ commitRecords }) => commitRecords([{ kind: 'printer', record: { ...(recordOf(getState(), 'machine', printer) as PrinterRecord), units: stored.units, slots: stored.slots } }], { label: `Undo detach ${unit}` }))
      .catch((e: Error) => toast('unit_restore_failed', { unit, detail: e.message })),
  } });
}
