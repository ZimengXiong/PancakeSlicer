import type { State } from '../state/store';
import type { Config } from '../api/types';
import type { ProfileKey } from './identity';
import { recordOf } from './view';
import { sameRecord, type PrinterRecord } from './records';
import type { ProfileRecovery, SwapMode } from './ownership';
import { recordWithStrip, stripOf } from './slots';

export type MaterialLibraryState = State;
export interface PrinterMaterialsPlan {
  printer: ProfileKey;
  filamentEdits: Record<ProfileKey, Config>;
  recoveries: ProfileRecovery[];
}
const filamentId = (s: State, key: string) => (recordOf(s, 'filament', key) ?? s.records.filaments.find((f) => f.aliases?.includes(key)))?.id ?? key;

const PAUSES = ['M600', 'M0', 'M1', 'M25', 'PAUSE', 'M601'];
export function machineSwap(config: Config, vendor: string | undefined): { mode: SwapMode; modes: SwapMode[] } {
  const nozzles = [config.nozzle_diameter ?? []].flat();
  if (nozzles.length > 1 && vendor !== 'BBL') return { mode: 'toolheads', modes: ['toolheads'] };
  const text = (v: Config[string] | undefined) => String(Array.isArray(v) ? v[0] ?? '' : v ?? '').trim();
  const change = text(config.change_filament_gcode), pause = text(config.machine_pause_gcode);
  const onlyPause = !change || change === pause || PAUSES.includes(change.toUpperCase());
  if (vendor === 'BBL' || text(config.single_extruder_multi_material) === '1' && !onlyPause) return { mode: 'ams', modes: ['ams', 'manual'] };
  return { mode: 'manual', modes: ['manual'] };
}

export function stripRecord(s: MaterialLibraryState): PrinterRecord | undefined {
  const record = recordOf(s, 'machine', s.printer) as PrinterRecord | undefined;
  if (!record || record.archived) return undefined;
  const strip = stripOf(s);
  const fill = record.swap === 'toolheads' && !Object.keys(strip.mixes).length ? Math.max(0, record.nozzles.length - strip.filaments.length) : 0;
  const filaments = [...strip.filaments, ...Array<string>(fill).fill(strip.filaments[0] ?? '')].map((key) => key ? filamentId(s, key) : '');
  const { process: _p, ...rest } = recordWithStrip(s, { ...strip, filaments, colours: [...strip.colours, ...Array<undefined>(fill)] }) ?? record;
  const process = s.process ? recordOf(s, 'process', s.process)?.id ?? s.process : record.process;
  const next: PrinterRecord = { ...rest, ...(process ? { process } : {}) };
  return sameRecord(next, record) ? undefined : next;
}

export function planPrinterSwitch(s: MaterialLibraryState, printerKey: string): PrinterMaterialsPlan {
  const record = recordOf(s, 'machine', printerKey) as PrinterRecord | undefined;
  const result: PrinterMaterialsPlan = { printer: record?.id ?? printerKey,
    filamentEdits: s.filamentEdits, recoveries: [] };
  if (!record) result.recoveries.push({ type: 'machine', key: printerKey, reason: 'missing-owner' });
  for (const slot of record?.slots ?? []) if (slot.filament && !recordOf(s, 'filament', slot.filament))
    result.recoveries.push({ type: 'filament', key: slot.filament, reason: 'missing-profile' });
  return result;
}
