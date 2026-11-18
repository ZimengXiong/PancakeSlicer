import { newSpeedModel, speedModelOf, SPEED_FEATURES, type SpeedModel } from './speed-rules';
import type { Config, ConfigValue, PresetType } from '../api/types';
import { switchPreset } from '../actions';
import { authoredChanges } from '../state/changes';
import { getState, presetEdits, resolvePreset, setState, toast, type State } from '../state/store';
import { clampHeight, type ScaleRow } from './auto-rules';
import { effectiveLayerHeight, printerOverrides } from './context';
import { acknowledgeProfileSave, captureProfileSave, profileSaves } from './project-draft';
import { LibraryWriteError } from './library-writes';
import { commitRecords, removeRecords } from './transactions';
import rules from '../../../server/profile_rules.json';
import { calibrationFor, calibrationTarget, slotNozzle } from './context';
import { ownValues, recordKind, recordOf, withOwnValues, type ProfileRecord } from './view';
import { getRecord, recordKey, type AnyRecord, type CalibrationRecord, type PrinterRecord, type ProcessRecord, type RecordKind, NAME_MAX, nameLength } from './records';
import { listProfiles } from './catalog';
import { slotFilaments } from './slots';
import { randomId } from '../random-id';

function calibrationWith(s: State, filament: string, slot: number, values: Config): CalibrationRecord {
  const row = calibrationFor(s, filament, slot), pid = s.printer;
  if (!recordOf(s, 'machine', pid)) throw new Error('Pick a printer first!');
  return row ? { ...row, revision: row.revision + 1, values: { ...row.values, ...values } } as CalibrationRecord
    : { id: `calibration:${randomId()}`, revision: 0, filament, printer: pid, ...calibrationTarget(s, pid, slotNozzle(s, pid, slot)), values };
}
const scalesOfRecord = (r: ProfileRecord | undefined): ScaleRow[] => (r as ProcessRecord | undefined)?.scales ?? [];
const speedModelOfRecord = (r: ProfileRecord | undefined) => (r as ProcessRecord | undefined)?.speedModel;

const selectedKey = (s: State, type: PresetType, slot: number) => (type === 'machine' ? s.printer : type === 'process' ? s.process : slotFilaments(s)[slot]) ?? '';
const message = (e: unknown) => e instanceof Error ? e.message : String(e);

export function autoOf(source: unknown, scales: ScaleRow[], speedModel?: SpeedModel | null): { auto?: object } {
  const prior = (source as { auto?: { preferredHeight?: number; heightRule?: string; speedModel?: SpeedModel } } | undefined)?.auto;
  const model = speedModel === null ? undefined : speedModel ?? speedModelOf(source) ?? (scales.length ? newSpeedModel() : undefined);
  return scales.length || prior?.preferredHeight || prior?.heightRule || model ? { auto: { schema: 3, scales, ...(prior?.preferredHeight ? { preferredHeight: prior.preferredHeight } : {}), ...(prior?.heightRule ? { heightRule: prior.heightRule } : {}), ...(model ? {speedModel: model} : {}) } } : {};
}
export type ValueChoice = 'formula' | 'fix' | 'project' | 'printer' | 'calibration' | 'filament';
export interface ValueSource { source: 'formula' | 'printer' | 'calibration' | 'filament'; row?: ScaleRow; printer?: string; filament?: string }
export const CALIBRATION_KEYS = new Set<string>(rules.calibrationKeys);
export const isCalibrationKey = (k: string) => CALIBRATION_KEYS.has(k) || rules.calibrationPrefixes.some((p) => k.startsWith(p));
export const scaleContext = (s: State) => {
  const nozzle = (s.records.printers.find((p) => p.id === s.printer)?.nozzles[0]?.diameter ?? 0);
  return { nozzle, layerHeight: effectiveLayerHeight(s) };
};
export function valueSources(s: State, type: PresetType, slot: number): Record<string, ValueSource> {
  if (type === 'filament') {
    const printer = recordOf(s, 'machine', s.printer)?.name ?? 'this printer', filament = recordOf(s, 'filament', selectedKey(s, type, slot))?.name ?? 'filament';
    return Object.fromEntries(Object.keys(authoredChanges(s, type, slot)).map((key) =>
      [key, { source: CALIBRATION_KEYS.has(key) ? 'calibration' as const : 'filament' as const, printer, filament }]));
  }
  if (type !== 'process') return {};
  const p = recordOf(s, type, selectedKey(s, type, slot)), own = ownValues(type, p), printer = recordOf(s, 'machine', s.printer)?.name ?? 'printer';
  const out: Record<string, ValueSource> = Object.fromEntries(scalesOfRecord(p).map((row) => [row.key, { source: 'formula' as const, row }]));
  for (const k of Object.keys(printerOverrides(s))) if (!(k in own) && !(speedModelOfRecord(p) && SPEED_FEATURES.some(f => f.key === k))) out[k] = { source: 'printer', printer };
  return out;
}
const round = (v: number, d = 4) => Math.round(v * 10 ** d) / 10 ** d;
export function solveFactor(row: ScaleRow, value: ConfigValue, ctx: { nozzle: number; layerHeight: number }): number | undefined {
  const v = Number([value].flat()[0]), h = clampHeight(ctx.layerHeight, ctx.nozzle);
  if (!Number.isFinite(v) || !(ctx.nozzle > 0) || !(h > 0)) return undefined;
  if (row.basis === 'nozzle') return round(v / ctx.nozzle);
  if (row.basis === 'layer') return round(v / h);
  const n = Math.round(v);
  return n < 1 ? undefined : round(Math.floor((n - 1) * h * 100 + 1e-6) / 100 + 0.01, 2);
}
export const formulaText = (row: ScaleRow) => row.basis === 'nozzle' ? `${row.factor} × nozzle` : row.basis === 'layer' ? `${row.factor} × layer height` : `${row.factor} mm ÷ layer height`;
export const defaultChoice = (v: ValueSource): ValueChoice => v.source === 'formula' ? 'formula' : v.source === 'calibration' ? 'calibration' : v.source === 'filament' ? 'filament' : 'printer';

function applyChoices(scales: ScaleRow[], deltas: Config, sources: Record<string, ValueSource>, choices: Record<string, ValueChoice>, ctx: { nozzle: number; layerHeight: number }) {
  const own: Config = {}, toPrinter: Config = {}, kept: Config = {}, toCalibration: Config = {};
  let rows = scales;
  for (const [k, v] of Object.entries(deltas)) {
    const src = sources[k], choice = src ? choices[k] ?? defaultChoice(src) : undefined;
    if (!src) own[k] = v;
    else if (choice === 'project') kept[k] = v;
    else if (choice === 'calibration') (isCalibrationKey(k) ? toCalibration : kept)[k] = v;
    else if (choice === 'filament') own[k] = v;
    else if (src.source === 'printer') toPrinter[k] = v;
    else if (choice === 'fix') { rows = rows.filter((r) => r.key !== k); own[k] = v; }
    else { const factor = solveFactor(src.row!, v, ctx); if (factor === undefined) kept[k] = v; else rows = rows.map((r) => r.key === k ? { ...r, factor } : r); }
  }
  return { rows, own, toPrinter, kept, toCalibration };
}
function printerWith(s: State, values: Config): PrinterRecord | undefined {
  const printer = recordOf(s, 'machine', s.printer) as PrinterRecord | undefined;
  if (!printer || !Object.keys(values).length) return undefined;
  return { ...printer, printOverrides: { ...printer.printOverrides, ...values } };
}

export function savesInPlace(s: State, type: PresetType, slot: number): boolean {
  const p = recordOf(s, type, selectedKey(s, type, slot));
  return !!p && !p.archived;
}

function changesOf(s: State, type: PresetType, slot: number) {
  return { edits: presetEdits(s, type, slot), project: (type === 'filament' ? s.projectPresets?.filaments[slot] : s.projectPresets?.[type]) ?? null };
}
function restoreChanges(type: PresetType, slot: number, key: string, c: ReturnType<typeof changesOf>) {
  setState((st) => ({
    ...(type === 'filament' ? { filamentEdits: { ...st.filamentEdits, [key]: c.edits } } : { dirty: { ...st.dirty, [type]: c.edits } }),
    ...(st.projectPresets && c.project ? { projectPresets: type === 'filament'
      ? { ...st.projectPresets, filaments: st.projectPresets.filaments.map((f, i) => i === slot ? c.project! : f) }
      : { ...st.projectPresets, [type]: c.project } } : {}),
  }));
}

export async function saveToProfile(type: PresetType, slot = 0, choices: Record<string, ValueChoice> = {}): Promise<void> {
  const s = getState();
  if (!s.api) throw new Error('Offline right now—your changes are safely kept in this tab.');
  if (!profileSaves.can({ type: 'START', capture: '' })) throw new Error('A save is already in progress; your changes are kept.');
  const save = captureProfileSave(type, slot), source = recordOf(s, type, save.key);
  if (!source || source.archived) throw new Error('That profile isn\'t available right now, but your edits are safely kept here.');
  const before = changesOf(s, type, slot), printerBefore = recordOf(s, 'machine', s.printer);
  const scales = scalesOfRecord(source), { rows, own, toPrinter, kept, toCalibration } = applyChoices(scales, save.edits, valueSources(s, type, slot), choices, scaleContext(s));
  if (Object.keys(kept).length === Object.keys(save.edits).length) throw new Error('Your edits apply to this project by default. Choose where you\'d like to save them permanently.');
  const values: Config = { ...ownValues(type, source), ...own };
  for (const r of rows) delete values[r.key];
  const saved = { ...save.edits };
  for (const k of Object.keys(kept)) delete saved[k];
  let record: ProfileRecord = withOwnValues(type, source, values);
  if (type === 'process') record = { ...record, scales: rows.length ? rows : undefined } as ProcessRecord;
  const printer = printerWith(s, toPrinter), calibration = Object.keys(toCalibration).length ? calibrationWith(s, source.id, slot, toCalibration) : undefined;
  const libraryChanged = Object.keys(own).length > 0 || type !== 'filament';
  profileSaves.send({ type: 'START', capture: save.id });
  try {
    const library = await commitRecords([...(libraryChanged ? [{ kind: recordKind(type), record, base: source }] : []), ...(printer ? [{ kind: 'printer' as const, record: printer }] : []),
      ...(calibration ? [{ kind: 'calibration' as const, record: calibration }] : [])], { label: 'Save to profile', apply: (st) => acknowledgeProfileSave(st, save, saved) });
    profileSaves.send({ type: 'ACKED', capture: save.id, revision: library.revision });
  } catch (e) {
    profileSaves.send({ type: 'FAILED', capture: save.id, code: e instanceof LibraryWriteError ? e.code : 'unavailable' });
    throw e;
  }
  const targets = [...(libraryChanged ? [source.name] : []), ...(printer ? [printer.name] : []), ...(calibration ? [`${printerBefore?.name ?? 'printer'} calibration`] : [])];
  const priorCalibration = calibration ? calibrationFor(s, source.id, slot) : undefined;
  toast('saved_4', { value: targets.join(' and ') }, { action: { label: 'Undo', run: () => {
    const now = getState(), back: { kind: RecordKind; record: AnyRecord }[] = [];
    const at = (kind: RecordKind, r: AnyRecord) => ({ kind, record: { ...r, revision: (getRecord(now.records, recordKey(kind, r.id))?.revision ?? r.revision) } as AnyRecord });
    if (libraryChanged) back.push(at(recordKind(type), source));
    if (printer && printerBefore) back.push(at('printer', printerBefore));
    if (calibration) back.push(at('calibration', priorCalibration ? priorCalibration as CalibrationRecord : { ...calibration, values: {} }));
    void commitRecords(back, { label: 'Undo save' }).then(() => restoreChanges(type, slot, save.key, before), (e) => toast('undo_save_failed', { detail: message(e) }));
  } } });
}

export const newProfileName = (s: State, type: PresetType, slot: number) => {
  const r = resolvePreset(s, type, selectedKey(s, type, slot)), name = r.record?.name ?? r.base?.name ?? '';
  return name.startsWith('My ') ? name : `My ${name}`;
};

export function newProfileNameError(s: State, type: PresetType, name: string): string | null {
  const n = name.trim();
  if (!n) return 'Enter a name';
  if (nameLength(n) > NAME_MAX) return `Keep the name to ${NAME_MAX} characters`;
  return listProfiles(s, type, { browseAll: true }).some((p) => p.name === n) ? `“${n}” is already taken` : null;
}

export type NewKind = 'same' | 'dynamic' | 'regular';
export async function saveAsNewProfile(type: PresetType, slot: number, name: string, kind: NewKind = 'same', choices: Record<string, ValueChoice> = {}): Promise<void> {
  const s = getState();
  if (!s.api) throw new Error('Offline right now—your changes are safely kept in this tab.');
  const error = newProfileNameError(s, type, name);
  if (error) throw new Error(error);
  const key = selectedKey(s, type, slot), resolved = resolvePreset(s, type, key);
  if (resolved.error) throw new Error(resolved.error);
  const deltas = authoredChanges(s, type, slot);
  const before = changesOf(s, type, slot), source = recordOf(s, type, key), ctx = scaleContext(s);
  const scales = scalesOfRecord(source), dynamic = type === 'process' && kind === 'dynamic' && !!speedModelOfRecord(source);
  const chosen = dynamic ? applyChoices(scales, deltas, valueSources(s, type, slot), choices, ctx) : undefined;
  const frozen = type === 'process' && kind === 'regular' ? { ...Object.fromEntries(scales.flatMap((r) => resolved.config[r.key] === undefined ? [] : [[r.key, resolved.config[r.key]!]])), ...(speedModelOfRecord(source) ? Object.fromEntries(SPEED_FEATURES.flatMap(f => [f.key, f.acceleration]).filter(k => resolved.config[k] !== undefined).map(k => [k, resolved.config[k]])) : {}), layer_height: String(ctx.layerHeight) } : {};
  const own = chosen ? chosen.own : { ...frozen, ...deltas };
  if (!source) throw new Error('Pick a profile from your library first!');
  const copyValues: Config = { ...ownValues(type, source), ...own };
  if (chosen) for (const r of chosen.rows) delete copyValues[r.key];
  const { archived: _a, ...base } = structuredClone(source);
  let copy: ProfileRecord = withOwnValues(type, { ...base, id: `profile:${randomId()}`, name: name.trim(), revision: 0 } as ProfileRecord, copyValues);
  if (type === 'process') copy = { ...copy, scales: chosen ? chosen.rows : kind === 'regular' ? undefined : (copy as ProcessRecord).scales, speedModel: kind === 'regular' ? undefined : (copy as ProcessRecord).speedModel } as ProcessRecord;
  await commitRecords([{ kind: recordKind(type), record: copy }], { label: 'Save as new profile' });
  const created = copy.id;
  const printer = chosen && printerWith(getState(), chosen.toPrinter);
  if (printer) await commitRecords([{ kind: 'printer', record: printer }], { label: 'Printer overrides' });
  switchPreset(type, slot, created, 'discard');
  if (chosen && Object.keys(chosen.kept).length) setState((st) => ({ dirty: { ...st.dirty, process: { ...st.dirty.process, ...chosen.kept } } }));
  toast('saved_as', { value: name.trim() }, { action: { label: 'Undo', run: () => {
    switchPreset(type, slot, key, 'discard');
    restoreChanges(type, slot, key, before);
    void removeRecords(type === 'machine' ? 'printer' : type, [created]).catch((e: unknown) => toast('undo_save_as_failed', { detail: message(e) }));
  } } });
}
