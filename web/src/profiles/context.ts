import type { Config } from '../api/types';
import type { State } from '../state/store';
import type { CalibrationRow } from './ownership';
import { slotExtruder } from '../printers/feeds';
import type { PrinterRecord, ProcessRecord } from './records';

export type ProfileContextState = State;
export type SettingOrigin = 'set' | 'auto' | 'printer';

const printer = (s: State, id = s.printer) => s.records.printers.find((p) => p.id === id) as PrinterRecord | undefined;

export function preferredHeight(s: State, key: string): number | undefined {
  return (s.records.processes.find((p) => p.id === key) as ProcessRecord | undefined)?.preferredHeight;
}
export function effectiveLayerHeight(s: State): number {
  return s.processVariant?.layerHeight ?? s.resolved.answer?.print.heights?.chosen ?? 0.2;
}
export function availableHeights(s: State): number[] {
  return s.resolved.answer?.print.heights?.available ?? [];
}
const noOverrides: Config = Object.freeze({}) as Config;
const overridesOf = new WeakMap<object, Config>();
export function printerOverrides(s: State): Config {
  const record = printer(s);
  if (!record?.printOverrides) return noOverrides;
  let value = overridesOf.get(record);
  if (!value) {
    const { layer_height: _h, ...rest } = record.printOverrides;
    overridesOf.set(record, (value = Object.freeze(rest) as Config));
  }
  return value;
}
export const printerName = (s: State) => printer(s)?.name ?? '';

export function calibrationTarget(s: State, printerKey: string, extruder: number): Pick<CalibrationRow, 'extruder' | 'nozzle' | 'volume'> {
  const n = printer(s, printerKey)?.nozzles[extruder];
  return { extruder, nozzle: n?.diameter ?? 0, ...(n?.volume ? { volume: n.volume } : {}) };
}
export function slotNozzle(s: State, printerKey: string, slot: number): number {
  const p = printer(s, printerKey);
  return slotExtruder(p, p?.nozzles.length ?? 1, slot) ?? 0;
}
export function calibrationAt(s: State, key: string, printerKey: string, extruder: number): CalibrationRow | undefined {
  const target = calibrationTarget(s, printerKey, extruder);
  return s.records.calibrations.find((c) => c.filament === key && c.printer === printerKey && c.extruder === extruder
    && Math.abs(c.nozzle - target.nozzle) < 1e-6 && (c.volume ?? '') === (target.volume ?? '')) as CalibrationRow | undefined;
}
export function calibrationFor(s: State, key: string, slot = 0, printerKey = s.printer): CalibrationRow | undefined {
  return calibrationAt(s, key, printerKey, slotNozzle(s, printerKey, slot));
}
