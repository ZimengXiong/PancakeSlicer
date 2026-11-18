import type { PresetType } from '../api/types';
import type { State } from '../state/store';
import type { ProfileRecovery } from './ownership';
import { getRecord, recordKey, type LibraryRecords, type PrinterRecord } from './records';
import { slotFilaments } from './slots';

const kindOf = (type: PresetType) => type === 'machine' ? 'printer' as const : type === 'process' ? 'process' as const : 'filament' as const;
export function prepareProfileLibraryHydration(s: State, records: LibraryRecords, options: { startup?: boolean; remembered?: string } = {}): Partial<State> {
  const ids = [...records.printers, ...records.filaments, ...records.processes].map((r) => r.id);
  if (new Set(ids).size !== ids.length) throw new Error('Oops! Your library has duplicate profile IDs. Export a backup copy before running recovery.');
  const find = (type: PresetType, key: string) => key ? getRecord(records, recordKey(kindOf(type), key)) as { id: string; archived?: unknown } | undefined
    ?? (type === 'filament' ? records.filaments.find((f) => f.aliases?.includes(key)) : undefined) : undefined;
  const live = records.printers.filter((p) => !p.archived);
  let printer = s.printer, process = s.process;
  if (options.startup && !s.profileSnapshots) {
    const remembered = options.remembered ? find('machine', options.remembered) : undefined;
    printer = remembered && !remembered.archived ? remembered.id : live.length === 1 ? live[0].id : '';
  }
  const record = printer ? find('machine', printer) as PrinterRecord | undefined : undefined;
  if (record && !record.archived && !s.profileSnapshots) process = record.process ?? (options.startup ? '' : process);
  else if (options.startup && !s.profileSnapshots && !find('process', process)) process = '';
  const filamentSlots = slotFilaments({ records, printer });
  const recoveries: ProfileRecovery[] = [];
  for (const [type, key] of [['machine', printer], ['process', process], ...filamentSlots.map((key) => ['filament', key])] as [PresetType, string][]) {
    if (!key) continue;
    const found = find(type, key);
    if (!found) recoveries.push({ type, key, reason: 'missing-profile' });
    else if (found.archived) recoveries.push({ type, key, reason: 'archived' });
  }
  return { records, profileLibraryRevision: records.revision, printer, process, profileRecoveries: recoveries };
}
