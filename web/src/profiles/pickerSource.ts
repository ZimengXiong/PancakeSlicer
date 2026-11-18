import type { PresetType } from '../api/types';
import { getState } from '../state/store';
import * as catalog from './catalog';
import { profileInfo } from './view';
import type { PrinterRecord } from './records';

export interface PickerEntry {
  key: string;
  name: string;
  vendor: string;
  material: string;
  model?: string;
  nozzle?: number;
  mine: boolean;
  reason?: string;
}

export function listProfiles(type: PresetType): PickerEntry[] {
  const s = getState();
  return catalog.listProfiles(s, type).map((row) => {
    const info = profileInfo(s, type, row.key), printer = info.record && type === 'machine' ? info.record as PrinterRecord : undefined;
    return { key: row.key, name: row.name, vendor: info.vendor ?? '', material: info.entry?.type ?? '', model: info.model, nozzle: printer?.nozzles[0]?.diameter, mine: true };
  });
}
