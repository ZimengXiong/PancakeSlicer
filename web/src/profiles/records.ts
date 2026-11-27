import type { Config } from '../api/types';
import type { FeedUnit } from '../printers/feeds';
import type { ScaleRow } from './auto-rules';
import type { SpeedModel } from './speed-rules';

export const LIBRARY_SCHEMA = 14;

export interface ArchivedMark { at: number; operationId: string }
interface RecordBase { id: string; name: string; revision: number; archived?: ArchivedMark; edited?: string }

export interface NozzleRecord { variant: string; diameter: number; volume?: string; type?: string; offset?: [number, number] }
export interface NozzlePreset { diameter: number; variants: string[] }
export interface SlotRecord { id: string; filament: string; colour?: string; tray?: string }
export type SwapMode = 'ams' | 'toolheads' | 'manual';
export interface MixRecord { id: string; components: string[]; ratios: number[]; gradient: 0 | 1 | null; perPart: boolean }
export const NAME_MAX = 200;
export const nameLength = (name: string) => [...name].length;
export const INSTALLED: Required<Pick<NozzleRecord, 'type' | 'volume'>> = { type: 'Direct Drive', volume: 'Standard' };
export interface PrinterRecord extends RecordBase {
  vendor: string; model: string; nozzles: NozzleRecord[];
  nozzlePresets?: NozzlePreset[];
  machine?: Config;
  printOverrides?: Config;
  process?: string; swap?: SwapMode; slots: SlotRecord[]; units?: FeedUnit[]; mixes?: MixRecord[];
  plate?: string;
  flushing?: FlushingRecord;
}
export interface FlushingRecord { multipliers?: number[]; volumes?: (number | null)[] }
export interface FilamentRecord extends RecordBase {
  product: string;
  settings?: Config;
  aliases?: string[];
}
export interface CalibrationRecord {
  id: string; revision: number; filament: string; printer: string; extruder: number; nozzle: number; volume?: string; values: Config;
}
export interface ProcessRecord extends RecordBase {
  product?: string;
  values?: Config; scales?: ScaleRow[]; preferredHeight?: number; heightRule?: string; speedModel?: SpeedModel;
}
export interface LibraryRecords {
  schema: typeof LIBRARY_SCHEMA; revision: number; epoch?: string;
  printers: PrinterRecord[]; filaments: FilamentRecord[]; calibrations: CalibrationRecord[]; processes: ProcessRecord[];
}
export const emptyLibrary = (): LibraryRecords => ({ schema: LIBRARY_SCHEMA, revision: 0, printers: [], filaments: [], calibrations: [], processes: [] });

export type RecordKind = 'printer' | 'filament' | 'process' | 'calibration';
const lists = { printer: 'printers', filament: 'filaments', process: 'processes', calibration: 'calibrations' } as const;
export type AnyRecord = PrinterRecord | FilamentRecord | ProcessRecord | CalibrationRecord;
export const recordKey = (kind: RecordKind, id: string) => `${kind}:${id}`;
export function parseRecordKey(key: string): [RecordKind, string] {
  const at = key.indexOf(':');
  return [key.slice(0, at) as RecordKind, key.slice(at + 1)];
}
export function getRecord(doc: LibraryRecords, key: string): AnyRecord | undefined {
  const [kind, id] = parseRecordKey(key);
  return (doc[lists[kind]] as AnyRecord[]).find((r) => r.id === id);
}
export function setRecord(doc: LibraryRecords, key: string, value: AnyRecord | undefined): LibraryRecords {
  const [kind, id] = parseRecordKey(key), list = doc[lists[kind]] as AnyRecord[];
  const current = list.find((r) => r.id === id), others = list.filter((r) => r.id !== id);
  const next = value && { ...value, revision: current ? current.revision + 1 : value.revision ?? 0 };
  return { ...doc, [lists[kind]]: next ? [...others, next] : others };
}
const ordered = (value: unknown): unknown => Array.isArray(value) ? value.map(ordered) : value && typeof value === 'object'
  ? Object.fromEntries(Object.entries(value).filter(([, v]) => v !== undefined).sort(([a], [b]) => a.localeCompare(b)).map(([k, v]) => [k, ordered(v)])) : value;
export function sameRecord(a: AnyRecord | undefined, b: AnyRecord | undefined): boolean {
  if (!a || !b) return a === b;
  return JSON.stringify(ordered({ ...a, revision: 0, edited: undefined })) === JSON.stringify(ordered({ ...b, revision: 0, edited: undefined }));
}
