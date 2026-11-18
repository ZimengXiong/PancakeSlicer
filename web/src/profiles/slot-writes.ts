import { getState, subscribeStore, toast, type State } from '../state/store';
import { commitRecords } from './transactions';
import { recordWithStrip, stripOf, type Strip } from './slots';
import type { FlushingRecord } from './records';

export function writeStrip(strip: Strip, label = 'Printer setup', s: State = getState(), flushing?: FlushingRecord): boolean {
  let record;
  try {
    record = recordWithStrip(s, strip);
  } catch (e) {
    toast('slot_writes_failed', { detail: (e as Error).message });
    return false;
  }
  if (record) commitRecords([{ kind: 'printer', record: flushing ? { ...record, flushing } : record }], { label, optimistic: true })
    .catch((e: unknown) => toast('slot_write_not_saved', { label, detail: e instanceof Error ? e.message : String(e) }));
  return true;
}

export const editStrip = (edit: (strip: Strip) => Strip, label?: string) => { writeStrip(edit(stripOf(getState())), label); };

export function adoptStrip(strip: Strip | null): void {
  if (!strip) return;
  if (getState().profileLibraryLoad.state === 'ready') { writeStrip(strip, 'Undo'); return; }
  const stop = subscribeStore(() => { if (getState().profileLibraryLoad.state !== 'ready') return; stop(); writeStrip(strip, 'Undo'); });
}
