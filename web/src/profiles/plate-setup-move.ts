import type { FlushingRecord } from './records';
import { getState, subscribeStore, toast, type State } from '../state/store';
import { commitRecords } from './transactions';
import { openingStep } from '../flow/project';
import { plateSetup } from './plate-setup';

export function adoptStoredPlateSetup(stored: { plate: string; flushing: FlushingRecord | null }, printer: string, from: string): void {
  if (!stored.plate && !stored.flushing) return;
  const answered = (s: State) => s.profileLibraryLoad.state === 'ready' && s.resolved.answeredFor?.printer === printer && s.resolved.state !== 'resolving';
  const apply = () => {
    const s = getState(), record = s.records.printers.find((p) => p.id === printer);
    if (!record) return;
    const kept: string[] = [], plates = plateSetup(s, printer)?.plates ?? [];
    let next = record;
    if (stored.plate && !plates.some((p) => p.value === stored.plate)) kept.push(`build plate ${stored.plate} (${record.name} doesn't take it)`);
    else if (stored.plate && !record.plate) next = { ...next, plate: stored.plate };
    else if (stored.plate && record.plate !== stored.plate) kept.push(`build plate ${stored.plate}`);
    if (stored.flushing && !record.flushing) next = { ...next, flushing: stored.flushing };
    else if (stored.flushing && JSON.stringify(record.flushing) !== JSON.stringify(stored.flushing)) kept.push('flushing volumes');
    if (next !== record) openingStep(() => { commitRecords([{ kind: 'printer', record: next }], { label: 'Move plate setup to the printer', optimistic: true })
      .catch((e) => toast('xs_build_plate_flushing', { from, name: record.name, detail: e instanceof Error ? e.message : String(e) })); });
    if (kept.length) toast('xs_stayed_off_keeping', { from, value: kept.join(' and '), name: record.name });
  };
  if (answered(getState())) { apply(); return; }
  const stop = subscribeStore(() => { if (!answered(getState())) return; stop(); apply(); });
}
