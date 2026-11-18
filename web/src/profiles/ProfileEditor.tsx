import { Suspense, useCallback, useEffect, useMemo, useRef, useState, type ReactNode } from 'react';
import { preloadable } from '../preloadable';
import type { Config, ConfigValue, PresetType } from '../api/types';
import { SettingsTab, type ProfileFields } from '../components/SettingsTab';
import { effectiveMode, getState, resolvePreset, toast, useStore } from '../state/store';
import { changedKeys } from './model';
import { rebasedProfileDraft } from './shared-draft';
import { commitRecords, recordCopy } from './transactions';
import { findProfile } from './identity';
import type { PrinterRecord, ProcessRecord } from './records';
import { modelLabel, profileSummary } from './metadata';
import { SaveMenu } from './SaveMenu';
import { isCalibrationKey, newProfileNameError } from './save-to-profile';
import { calibrationFor, calibrationTarget, slotNozzle } from './context';
import type { CalibrationRow } from './ownership';
import { MenuButton } from '../components/Menu';
import { NozzleSize } from './NozzleSize';
import { BuildPlateField } from './SetupRows';
import { MIXED_SIZES } from './record-edits';
import { acknowledgeProfileSave, type ProfileSave } from './project-draft';
import { requestPreset } from '../components/PresetCombo';
import { sameValue } from '../state/value-equality';
import { Glyph } from '../components/Glyph';
import { useMachine } from '../flow/fsm';
import { createResolvedSnapshot, resolveRequestOf, type ResolveRequest } from '../flow/resolved';
import { normalizeConfig } from '../config-values';
import { libraryWrites } from './library-writes';
import { recordKey } from './records';
import { recordProblems } from '../persist/profile-library';
import { movesText, profileSaveFor } from '../flow/profile-save';
import { librarySync } from '../flow/library-sync';
import { settingSources } from './bases';
import { DEFAULT_SCALES, effectiveShells, type ScaleRow } from './auto-rules';
import { confirmDialog } from '../components/Ask';
import { DynamicTab } from './DynamicTab';
import { OverrideList, PrintOverrides } from './PrintOverrides';
import { FilamentPrinters, usePrinterTuning } from './FilamentPrinters';
import { filamentScope, type PrinterSide } from '../flow/filament-scope';
import { newSpeedModel, SPEED_KEYS, type SpeedModel } from './speed-rules';
import { effectiveLayerHeight, type SettingOrigin } from './context';
import { askedValues, ownValues, recordKind, recordOf, selectionValues, useAskedValues, valueBelow, withOwnValues, type ProfileRecord } from './view';
import { plateSlots } from '../printers/slot-check';
import { extruderLabel } from '../multinozzle/config';
import { DynamicBadge } from '../components/DynamicBadge';
import { Sep, joinParts } from '../separator';
import { slotFilaments } from './slots';
import { randomId } from '../random-id';
const ConnectionEditor = preloadable(() => import('../printers/ConnectionEditor').then(m => ({default: m.ConnectionEditor})));
export interface ProfileDraft { speedModel?: SpeedModel | null; speedOverrides?: Config; scales?: ScaleRow[];  printOverrides?: Config;  cleared?: string[];  locked?: string[]; values: Config; baseline: Config; origin: import("./view").ProfileRecord | undefined; printer: string;staged?:ProfileSave}
export function acknowledgedProfileDraft(current:ProfileDraft,submitted:ProfileDraft,origin:ProfileRecord,baseline:Config):ProfileDraft|null {
  const keys=new Set([...Object.keys(current.values),...Object.keys(submitted.values)]);
  const unchanged=[...keys].every(key=>sameValue(key,current.values[key],submitted.values[key]));
  if(unchanged && JSON.stringify(current.scales)===JSON.stringify(submitted.scales) && JSON.stringify(current.speedModel)===JSON.stringify(submitted.speedModel) && JSON.stringify(current.speedOverrides)===JSON.stringify(submitted.speedOverrides) && JSON.stringify(current.printOverrides)===JSON.stringify(submitted.printOverrides) && current.staged===submitted.staged && current.printer===submitted.printer)return null;
  const values={...baseline};
  for(const key of keys)if(!sameValue(key,current.values[key],submitted.values[key])){
    if(current.values[key]===undefined)delete values[key];else values[key]=current.values[key];
  }
  return {...current,values,baseline,origin};
}
export function ProfileEditor({type, name, draft, update, onBack, onClose, onSavedAs, onSavingChange, onRemove, onDuplicate, foldActions = false, renameRequest}: {type: PresetType; name: string; draft?: ProfileDraft; renameRequest?: {key: string; at: number}; update(key: string, draft: ProfileDraft | null): void; onBack(): void; onClose?(): void; onSavedAs?(key: string): void; onRemove(): void; onDuplicate?(): void; onSavingChange?(saving: boolean): void; foldActions?: boolean}) {
  const records = useStore(s => s.records);
  const ready=useStore(s=>s.profileLibraryLoad.state==='ready');
  const problem=useStore(s=>recordProblems(s,type,name,recordOf(s,type,name)?.name ?? '').join('\n') || undefined);
  const ownerPrinter = useRef(draft?.printer ?? getState().printer).current;
  const selected = useStore((st) => type === 'machine' ? st.printer === name : type === 'process' ? st.process === name : slotFilaments(st).includes(name));
  useAskedValues(selected ? null : {type, id: name});
  const resolved = useStore((st) => resolvePreset(st, type, name));
  const source = useMemo(() => recordOf(getState(), type, name), [records, type, name]);
  const key = JSON.stringify([type, name]);
  const profileName = source?.name ?? resolved.base?.name ?? 'Unavailable profile';
  const baseline = draft?.baseline ?? resolved.config, values = draft?.values ?? resolved.config;
  const changes = useMemo(() => changedKeys(values, baseline), [values, baseline]);
  const mode=useStore(effectiveMode);
  const scopeSnap = useMachine(filamentScope(name));
  const connections=useStore(s=>s.printerConnections.enabled);
  const m = useMemo(() => profileSaveFor(key), [key]), snap = useMachine(m);
  const fieldsRef = useRef<HTMLFieldSetElement>(null);
  useEffect(() => { const visible = () => !!fieldsRef.current?.getClientRects().length && document.visibilityState === 'visible';
    librarySync.send({type: 'EDITOR_SHOWN', key, visible}); return () => librarySync.send({type: 'EDITOR_HIDDEN', key, visible}); }, [key]);
  const saving = snap.value === 'saving' || snap.value === 'renaming', error = snap.value === 'failed' ? snap.context.error : null;
  const setError = (message: string | null) => m.send(message ? {type: 'REJECT', error: message} : {type: 'DISMISS'});
  useEffect(() => {onSavingChange?.(saving);}, [saving]);
  useEffect(() => () => {if (m.get().value === 'saving' || m.get().value === 'renaming') onSavingChange?.(false);}, []);
  const latest = useRef<ProfileDraft>({values, baseline, origin: draft?.origin ?? source, printer: draft?.printer ?? ownerPrinter, staged:draft?.staged}); latest.current = {values, baseline, origin: draft?.origin ?? source, printer: draft?.printer ?? ownerPrinter, staged:draft?.staged,scales:draft?.scales,printOverrides:draft?.printOverrides,speedModel:draft?.speedModel,speedOverrides:draft?.speedOverrides,cleared:draft?.cleared,locked:draft?.locked};
  const set = useCallback((k: string, value: ConfigValue) => { const current = latest.current; update(key, {...current, values: {...current.values, [k]: value}}); }, [key, update]);
  const reset = useCallback((k: string) => { const current = latest.current; const next = {...current.values}; if (current.baseline[k] === undefined) delete next[k]; else next[k] = current.baseline[k]; update(key, {...current, values: next}); }, [key, update]);
  const resetToBase = useCallback((k: string) => { const current = latest.current, next = {...current.values}, below = valueBelow(resolved.layers[k], 'record');
    if (below === undefined) delete next[k]; else next[k] = below;
    update(key, {...current, values: next, cleared: [...new Set([...current.cleared ?? [], k])]}); }, [key, update, resolved.layers]);
  const resetAllToBase = () => { const current = latest.current, next = {...current.values}, keys = Object.keys(ownValues(type, source));
    for (const k of keys) { const below = valueBelow(resolved.layers[k], 'record'); if (below === undefined) delete next[k]; else next[k] = below; }
    update(key, {...current, values: next, cleared: [...new Set([...current.cleared ?? [], ...keys])], ...(type === 'process' ? {speedOverrides: {}} : {})}); };
  const savedScales: ScaleRow[] = (source as ProcessRecord | undefined)?.scales ?? [];
  const scales = type === 'process' ? draft?.scales ?? savedScales : [];
  const savedSpeedModel = (source as ProcessRecord | undefined)?.speedModel, speedModel = type === 'process' ? draft?.speedModel === null ? undefined : draft?.speedModel ?? savedSpeedModel : undefined;
  const savedSpeedOverrides: Config = Object.fromEntries(Object.entries(ownValues(type, source)).filter(([k]) => SPEED_KEYS.has(k)));
  const speedOverrides: Config = draft?.speedOverrides ?? {...savedSpeedOverrides, ...Object.fromEntries(Object.entries(changes).filter(([k]) => SPEED_KEYS.has(k)))};
  const dynamic = scales.length > 0 || !!speedModel;
  const speedDirty = JSON.stringify(speedModel) !== JSON.stringify(savedSpeedModel) || JSON.stringify(speedOverrides) !== JSON.stringify(savedSpeedOverrides);
  const put = (next: ProfileDraft) => {
    const same = (a: unknown, b: unknown) => JSON.stringify(a ?? null) === JSON.stringify(b ?? null);
    const clean = !Object.keys(changedKeys(next.values, next.baseline)).length && !next.cleared?.length && !next.locked?.length && !next.staged
      && same(next.scales ?? savedScales, savedScales) && (next.speedModel === undefined || same(next.speedModel, savedSpeedModel))
      && same(next.speedOverrides ?? savedSpeedOverrides, savedSpeedOverrides) && same(next.printOverrides ?? savedOverrides, savedOverrides);
    update(key, clean ? null : next);
  };
  const useMultiplier = (k: string) => {const next = {...speedOverrides}; delete next[k]; const current = latest.current, values = {...current.values};
    if (current.baseline[k] === undefined) delete values[k]; else values[k] = current.baseline[k];
    put({...current, values, speedOverrides: next});};
  const restoreOverride = (k: string) => {const current = latest.current, values = {...current.values};
    const next: Config = Object.fromEntries(Object.keys({...savedSpeedOverrides, ...speedOverrides}).flatMap((x) => x !== k ? (x in speedOverrides ? [[x, speedOverrides[x]]] : []) : x in savedSpeedOverrides ? [[x, savedSpeedOverrides[x]]] : []));
    if (current.baseline[k] === undefined) delete values[k]; else values[k] = current.baseline[k];
    put({...current, values, speedOverrides: next});};
  const setSpeedModel = (speedModel: SpeedModel) => update(key, {...latest.current, speedModel});
  const draftRecordOf = (submitted: ProfileDraft, current: ProfileRecord): ProfileRecord => {
    const edited = changedKeys(submitted.values, submitted.baseline);
    const own: Config = {...ownValues(type, current), ...edited};
    for (const k of submitted.locked ?? []) if (submitted.values[k] !== undefined) own[k] = submitted.values[k]!;
    for (const k of submitted.cleared ?? []) delete own[k];
    if (type !== 'process') return withOwnValues(type, current, own);
    const rows = submitted.scales ?? savedScales, model = submitted.speedModel === null ? undefined : submitted.speedModel ?? savedSpeedModel;
    if (rows.length || model) for (const k of SPEED_KEYS) {
      delete own[k];
      const explicit = submitted.speedOverrides ?? {...savedSpeedOverrides, ...Object.fromEntries(Object.entries(edited).filter(([k]) => SPEED_KEYS.has(k)))};
      if (explicit[k] !== undefined) own[k] = explicit[k] as Config[string];
    }
    return {...withOwnValues(type, current, own), scales: rows.length ? rows : undefined, speedModel: model} as ProcessRecord;
  };

  const scalesDirty = JSON.stringify(scales) !== JSON.stringify(type === 'process' ? savedScales : []);
  const nozzle = useStore((st) => st.records.printers.find((p) => p.id === st.printer)?.nozzles[0]?.diameter ?? 0.4);
  const height = useStore((st) => effectiveLayerHeight(st) ?? 0.2);
  const heightLimits = useStore((st) => { const c = resolvePreset(st, 'machine', st.printer).config; return `${[c.min_layer_height].flat()[0] ?? ''},${[c.max_layer_height].flat()[0] ?? ''}`; });
  const [minHeight, maxHeight] = heightLimits.split(',').map((v) => Number(v) || undefined);
  const savedOverrides = (source as {printOverrides?: Config} | undefined)?.printOverrides ?? {};
  const printAsk = type === 'machine' && !selected && getState().process ? {type: 'process' as const, id: getState().process, printer: name} : null;
  useAskedValues(printAsk);
  const printLayers = useStore((st) => type !== 'machine' ? undefined : selected ? selectionValues(st, 'process').layers : printAsk ? askedValues(st, printAsk).layers : undefined);
  const overrideDefaults = useMemo(() => Object.fromEntries(Object.entries(printLayers ?? {}).flatMap(([k, l]) => { const below = valueBelow(l, 'printer'); return below === undefined ? [] : [[k, below]]; })) as Config, [printLayers]);
  const overrides = type === 'machine' ? draft?.printOverrides ?? savedOverrides : {};
  const overridesDirty = type === 'machine' && JSON.stringify(overrides) !== JSON.stringify(savedOverrides);
  const setOverrides = useCallback((next: Config) => update(key, {...latest.current, printOverrides: next}), [key, update]);
  const setScales = useCallback((next: ScaleRow[]) => update(key, {...latest.current, scales: next}), [key, update]);
  const makeDynamic = () => { const current = latest.current;
    update(key, {...current, cleared: [...new Set([...current.cleared ?? [], ...DEFAULT_SCALES.map((r) => r.key)])], scales: [...DEFAULT_SCALES], speedModel: newSpeedModel()}); };
  const makeRegular = async () => {
    if (!await confirmDialog('Make regular', `Lock in the calculated values for a ${nozzle} mm nozzle and ${height.toFixed(2)} mm layer height?`, ['Make regular', 'Cancel'])) return;
    const current = latest.current;
    const locked = Object.fromEntries([...scales.map((r) => r.key), ...SPEED_KEYS].flatMap((k) => current.values[k] === undefined ? [] : [[k, current.values[k]!]]));
    update(key, {...current, values: {...current.values, ...locked, layer_height: String(height)}, locked: Object.keys(locked), scales: [], speedModel: null}); };
  const printer = useStore((st) => findProfile(st, 'machine', st.printer));
  const fromPrinter = type === 'process' ? (printer as {printOverrides?: Config} | undefined)?.printOverrides ?? {} : {};
  const printerName = printer?.name ?? '';
  const draftResolve = useMemo(() => createResolvedSnapshot(), [key]);
  const draftState = useMachine(draftResolve);
  const libraryRevision = useStore((st) => st.records.revision), selectionInputs = useStore((st) => st.resolved.answeredFor);
  const draftRequest = useMemo(() => {
    if (!draft || !source) return null;
    const base = resolveRequestOf(getState());
    if (!base) return null;
    const record = draftRecordOf(draft, source) as ProfileRecord & {scales?: ScaleRow[]; speedModel?: SpeedModel; preferredHeight?: number};
    const {revision: _revision, profiles: _profiles, objects: _objects, ...context} = base;
    return {...context, profiles: [{type, id: name, ...(type !== 'machine' ? {printer: draft.printer || ownerPrinter} : {})}],
      drafts: {records: {[source.id]: {values: ownValues(type, record), ...(type === 'process' ? {scales: record.scales, speedModel: record.speedModel, preferredHeight: record.preferredHeight} : {})}}}};
  }, [draft, source, libraryRevision, selectionInputs]); // eslint-disable-line react-hooks/exhaustive-deps
  const draftJson = draftRequest && JSON.stringify(draftRequest);
  useEffect(() => { if (draftRequest) draftResolve.send({type: 'INPUT', request: draftRequest as ResolveRequest}); }, [draftJson]); // eslint-disable-line react-hooks/exhaustive-deps
  const draftAnswer = draftJson && JSON.stringify(draftState.context.answered) === draftJson ? draftState.context.answer?.profiles?.[0] : undefined;
  const draftProfile = draftAnswer?.status.ok ? draftAnswer : undefined;
  const speedNozzleCount = useStore((st) => st.records.printers.find((p) => p.id === st.printer)?.nozzles.length ?? 1), speedVendor = useStore((st) => st.records.printers.find((p) => p.id === st.printer)?.vendor);
  const usedSlots = useStore((st) => plateSlots(st).join(','));
  const answerNozzles = draftProfile ? draftProfile.nozzles : resolved.nozzles;
  const usedNozzles = useMemo(() => { const used = usedSlots.split(',').map(Number); const all = answerNozzles ?? [];
    const on = all.filter((n) => n.slots.some((slot) => used.includes(slot + 1))); return on.length ? on : all.slice(0, 1); }, [answerNozzles, usedSlots]);
  const mixedFlow = useStore((st) => usedNozzles.some((n) => new Set(n.slots.filter((slot) => usedSlots.split(',').map(Number).includes(slot + 1))
    .map((slot) => String([selectionValues(st, 'filament', slot).values.filament_max_volumetric_speed].flat()[0] ?? ''))).size > 1));
  const speedResolution = { details: usedNozzles[0]?.speeds ?? {}, nozzles: usedNozzles, missingReference: !!speedModel && resolved.state === 'ready' && !resolved.nozzles?.length };
  const shown = useMemo(() => draftProfile ? {...values, ...normalizeConfig(draftProfile.values, true)} : values, [values, draftProfile]);
  const lifted = useMemo(() => type !== 'process' ? {} : Object.fromEntries((['top', 'bottom'] as const).flatMap((side) => {
    const e = effectiveShells(shown, side, height);
    return e.lifted ? [[`${side}_shell_layers`, `${e.effective} at ${height} mm to keep ≥ ${e.thickness} mm`]] : [];
  })), [type, shown, height]);
  const sourceText = useMemo(() => ({...Object.fromEntries(scales.map((r) => [r.key, r.basis === 'nozzle' ? `${r.factor} × nozzle ${nozzle}` : r.basis === 'layer' ? `${r.factor} × layer height ${height}` : `⌈ ${r.factor} mm ÷ layer height ${height} ⌉`])), ...Object.fromEntries(Object.keys(fromPrinter).map((k) => [k, `Set by ${printerName}`])), ...lifted, ...Object.fromEntries(Object.entries(speedResolution.details).map(([key, d]) => [key, joinParts([`${d.speed} mm/s`, d.override && 'Absolute override', d.limit && `capped by ${d.limit.toLowerCase()}`])]))}), [scales, nozzle, height, fromPrinter, printerName, lifted, speedResolution]);
  const sources = useMemo(() => {
    const base = settingSources(getState(), type, name);
    if (type !== 'process') return base;
    const out: Record<string, SettingOrigin> = {};
    for (const [k, v] of Object.entries(base)) if (v !== 'auto' || !savedScales.some((r) => r.key === k)) out[k] = v;
    for (const r of scales) out[r.key] = 'auto';
    for (const k of Object.keys(changes)) if (!scales.some((r) => r.key === k)) out[k] = 'set';
    const own = (source as {overlay?: Config} | undefined)?.overlay ?? {};
    for (const k of Object.keys(fromPrinter)) if (!(k in own) && !(k in changes)) out[k] = 'printer';
    for (const k of Object.keys(speedResolution.details)) { if ((k in speedOverrides) !== (k in savedSpeedOverrides)) out[k] = 'set'; else if (k in speedOverrides) delete out[k]; else out[k] = 'auto'; }
    for (const k of Object.keys(lifted)) if (!(k in changes)) out[k] = 'auto';
    return out;
  }, [type, name, records, scales, changes, fromPrinter, source, lifted, speedResolution]);
  const inSavedOrder = (rows: ScaleRow[]) => [...rows].sort((a, b) => { const i = savedScales.findIndex((r) => r.key === a.key), j = savedScales.findIndex((r) => r.key === b.key); return (i < 0 ? Infinity : i) - (j < 0 ? Infinity : j); });
  const setOrUnlink = useCallback((k: string, value: ConfigValue) => { const current = latest.current, rows = current.scales ?? savedScales;
    if (speedModel && SPEED_KEYS.has(k)) {
      const first = String(Array.isArray(value) ? value[0] ?? '' : value ?? '');
      if (first === 'nil' || first === '') { useMultiplier(k); return; }
      const unset = speedResolution.details[k]?.unset;
      if (k in savedSpeedOverrides && unset !== undefined && Math.abs(Number(first) - unset) < 1e-6) { restoreOverride(k); return; }
      put({...current, values: {...current.values, [k]: value}, speedOverrides: {...speedOverrides, [k]: value}}); return;
    }
    update(key, {...current, values: {...current.values, [k]: value}, ...(rows.some((r) => r.key === k) ? {scales: rows.filter((r) => r.key !== k)} : {})}); }, [key, update, savedScales, speedModel, speedOverrides, speedResolution]); // eslint-disable-line react-hooks/exhaustive-deps
  const relink = useCallback((k: string): void => {
    const row = savedScales.find((r) => r.key === k) ?? DEFAULT_SCALES.find((r) => r.key === k);
    if (type === 'process' && speedModel && SPEED_KEYS.has(k)) {useMultiplier(k); return;}
    if (type !== 'process' || !row) {resetToBase(k); return;}
    const current = latest.current, next = {...current.values}; if (current.baseline[k] === undefined) delete next[k]; else next[k] = current.baseline[k];
    update(key, {...current, values: next, scales: inSavedOrder([...(current.scales ?? savedScales).filter((r) => r.key !== k), row]), ...(k in ownValues(type, source) ? {cleared: [...new Set([...current.cleared ?? [], k])]} : {})});
  }, [type, key, update, savedScales, resetToBase, speedModel, useMultiplier]);
  const resetEdit = useCallback((k: string) => { const current = latest.current, rows = current.scales ?? savedScales, saved = savedScales.find((r) => r.key === k);
    if (type === 'process' && speedModel && SPEED_KEYS.has(k)) { restoreOverride(k); return; }
    if (type !== 'process' || !saved || rows.some((r) => r.key === k)) { reset(k); return; }
    const next = {...current.values}; if (current.baseline[k] === undefined) delete next[k]; else next[k] = current.baseline[k];
    update(key, {...current, values: next, scales: inSavedOrder([...rows, saved])}); }, [type, key, update, reset, savedScales, speedModel, restoreOverride]); // eslint-disable-line react-hooks/exhaustive-deps
  const overridden = useCallback((k: string) => dynamic && SPEED_KEYS.has(k) ? k in speedOverrides : undefined, [dynamic, speedOverrides]);
  const editorChanges = useMemo(() => { const moved = [...SPEED_KEYS].filter((k) => (k in speedOverrides) !== (k in savedSpeedOverrides));
    return moved.length ? {...changes, ...Object.fromEntries(moved.map((k) => [k, shown[k]]))} : changes; }, [changes, speedOverrides, savedSpeedOverrides, shown]);
  const editor: ProfileFields = useMemo(() => ({name, overridden, readOnlyKeys: dynamic ? SPEED_KEYS : undefined, config: shown, baseline, changes: editorChanges, set: type === 'process' ? setOrUnlink : set, reset: resetEdit, mode: type === 'process' ? 'develop' : mode, sources, sourceText, resetToBase: type === 'process' ? relink : resetToBase, state: resolved.state, error: resolved.error}), [name, overridden, shown, baseline, editorChanges, set, setOrUnlink, resetEdit, mode, type, sources, sourceText, resetToBase, relink, dynamic, resolved.state, resolved.error]);
  const dirty = Object.keys(changes).length > 0 || !!draft?.cleared?.length || !!draft?.locked?.length || scalesDirty || speedDirty || overridesDirty || !!draft?.staged;
  const revert = () => {update(key, null); setError(null);};
  const discarded = (e: unknown) => {if ((e as {code?: string}).code === 'discarded') update(key, null);};
  const conflicted = useMachine(libraryWrites, (w) => w.value === 'conflicted' && !!source && w.context.conflicts.includes(recordKey(recordKind(type), source.id)));
  const save = async (asNew?: string) => {
    if (!m.can({type: 'SAVE'}) || !dirty) return;
    const s = getState(), current = recordOf(s, type, name);
    if (asNew !== undefined) { const issue = newProfileNameError(s, type, asNew); if (issue) { setError(issue); return; } }
    const submitted={...latest.current,values:structuredClone(latest.current.values)};
    if (!s.api) {setError('Offline right now—your edits are safely kept in this tab.'); return;}
    if (!current || current.archived) {setError(resolved.error ?? 'That profile isn\'t available right now, but your edits are safely kept here.'); return;}
    m.send({type: 'SAVE'});
    const request = m.get().context.request;
    try {
      let next: ProfileRecord = draftRecordOf(submitted, current);
      if (type === 'machine' && submitted.printOverrides) next = {...next, printOverrides: submitted.printOverrides} as PrinterRecord;
      if (asNew !== undefined) next = recordCopy(recordKind(type), next, `profile:${randomId()}`, asNew.trim());
      await commitRecords([{kind: recordKind(type), record: next, ...(asNew === undefined && submitted.origin ? {base: submitted.origin} : {})}], {label: 'Save profile', apply: submitted.staged ? (state) => acknowledgeProfileSave(state, submitted.staged!, submitted.values) : undefined});
      if (asNew !== undefined) {
        update(key, null); setNewName(null); m.send({type: 'SAVED', request}); toast('saved_as', { value: asNew.trim() });
        if (submitted.staged) requestPreset(type, submitted.staged.slot, next.id);
        onSavedAs?.(next.id);
        return;
      }
      const origin = recordOf(getState(), type, name);
      if (!origin) throw new Error('This profile was removed while you were saving. Don\'t worry, your edits are still here!');
      update(key, acknowledgedProfileDraft(latest.current, submitted, origin, submitted.values)); m.send({type: 'SAVED', request}); toast('saved_2', { profileName });
    } catch (e) { discarded(e); m.send({type: 'FAIL', request, error: e instanceof Error ? e.message : String(e)}); }
  };
  const saveForPrinter = async () => {
    const tune = Object.fromEntries(Object.entries(changes).filter(([k]) => isCalibrationKey(k)));
    if (!m.can({type: 'SAVE'}) || !Object.keys(tune).length) return;
    const s = getState(), filament = recordOf(s, type, name), slot = Math.max(0, slotFilaments(s).indexOf(name));
    if (!filament || !s.api) { setError('Offline right now—your edits are safely kept in this tab.'); return; }
    const row = calibrationFor(s, name, slot, s.printer), submitted = latest.current;
    const next: CalibrationRow = row ? {...row, revision: row.revision + 1, values: {...row.values, ...tune}}
      : {id: `calibration:${randomId()}`, revision: 0, filament: filament.id, printer: s.printer, ...calibrationTarget(s, s.printer, slotNozzle(s, s.printer, slot)), values: {...tune}};
    m.send({type: 'SAVE'});
    const request = m.get().context.request;
    try {
      await commitRecords([{kind: 'calibration', record: next}], {label: 'Save for printer',
        apply: submitted.staged ? (state) => acknowledgeProfileSave(state, submitted.staged!, submitted.values) : undefined});
      const left = Object.fromEntries(Object.entries(changes).filter(([k]) => !isCalibrationKey(k)));
      update(key, Object.keys(left).length || scalesDirty || speedDirty || overridesDirty ? {...submitted, values: {...submitted.baseline, ...left}, staged: undefined} : null);
      m.send({type: 'SAVED', request}); toast('saved_3', { printerModel });
    } catch (e) { discarded(e); m.send({type: 'FAIL', request, error: e instanceof Error ? e.message : String(e)}); }
  };
  const [newName, setNewName] = useState<string | null>(null);
  const printerModel = useStore((st) => { const r = resolvePreset(st, 'machine', st.printer); return modelLabel(r.config, r.base?.vendor ?? '') || r.name || 'this printer'; });
  const renamable = !!source && !source.archived;
  const naming = snap.value === 'naming' || snap.value === 'renaming' ? snap.context.name : null, nameError = naming === null ? null : snap.context.nameError;
  const nameInput = useRef<HTMLInputElement>(null);
  const startRename = () => {if (renamable && m.can({type: 'RENAME', name: profileName})) m.send({type: 'RENAME', name: profileName});};
  useEffect(() => {if (renameRequest?.key === name && Date.now() - renameRequest.at < 2000) startRename();}, [renameRequest]);
  const writeName = async (next: string) => {
    const current = recordOf(getState(), type, name);
    if (!current) throw new Error('Oops! This profile isn\'t available right now.');
    const prior = current.name;
    await commitRecords([{kind: recordKind(type), record: {...current, name: next}}], {label: 'Rename'});
    const renamed = recordOf(getState(), type, name);
    if (draft && renamed) update(key, {...latest.current, origin: renamed});
    toast('renamed', { next }, { action: {label: 'Undo', run: () => {
      if (!m.can({type: 'RENAME', name: next})) {toast('rename_wait_for_save'); return;}
      m.send({type: 'RENAME', name: next}); m.send({type: 'TYPE', value: prior}); void commitName();
    }} });
  };
  const commitName = async () => {
    const v = m.get().value;
    if (v === 'renaming') {m.send({type: 'COMMIT', value: '', current: '', taken: false}); return;}
    if (v !== 'naming') return;
    const s = getState(), current = findProfile(s, type, name), value = m.get().context.name, next = value.trim();
    if (!current) return;
    const taken = (type === 'machine' ? s.records.printers : type === 'process' ? s.records.processes : s.records.filaments).some((p) => !p.archived && p.id !== current.id && p.name.toLocaleLowerCase() === next.toLocaleLowerCase());
    m.send({type: 'COMMIT', value, current: current.name, taken});
    if (m.get().value !== 'renaming') return;
    const request = m.get().context.request;
    try {await writeName(next); m.send({type: 'RENAMED', request});}
    catch (e) {m.send({type: 'RENAME_FAIL', request, error: e instanceof Error ? e.message : String(e)});}
  };
  if (type === 'filament' && scopeSnap.context.side && resolved.record) return <PrinterValuesEditor filament={resolved.record.id} side={scopeSnap.context.side} profileName={profileName} onClose={onClose}/>;
  const mixedSizes = type === 'machine' && new Set((source as PrinterRecord | undefined)?.nozzles.map((n) => n.diameter) ?? []).size > 1;
  if (resolved.record && resolved.error && resolved.state !== 'failed') return <DetailPane label={`Edit ${profileName}`} error={problem ?? (mixedSizes ? MIXED_SIZES : resolved.error)}
    identity={<span className="pf-editor-name"><strong>{profileName}</strong></span>} summary={null}
    controls={type === 'machine' && source ? <NozzleSize printerId={source.id} disabled={!ready}/> : null}>
    {!problem && !/^Pick a printer/.test(resolved.error) && <p className="opt-note">{type === 'machine' ? 'Tweak the nozzles above to settings your printer can slice with — the rest of its settings will pop back up once they resolve.' : 'Its settings will pop right back once the service can resolve them.'}</p>}
  </DetailPane>;
  if (!resolved.record || resolved.error) return <section className="pf-full-editor"><div className="pf-editor-error" role="alert"><strong>{resolved.record?.name ?? 'Profile unavailable'}</strong><p>{resolved.error ?? 'Oops! We couldn\'t load that profile.'}</p><button type="button" className="btn" onClick={() => void (async () => {try {const {loadCatalog}=await import('../actions');await loadCatalog();const s=getState();if(s.profileLibraryLoad.state==='failed')throw new Error(s.profileLibraryLoad.error);if(!findProfile(s,type,name))throw new Error('That profile is still unavailable.');} catch(e) {setError(e instanceof Error ? e.message : String(e));}})()}>Retry</button>{error && <p>{error}</p>}</div><button type="button" className="btn-ghost" onClick={onBack}>Close profiles</button></section>;
  return <DetailPane label={`Edit ${profileName}`} error={error ?? problem}
    identity={<>{naming !== null ? <input ref={nameInput} autoFocus onFocus={e => e.currentTarget.select()} className="pf-name-input" aria-label="Profile name" aria-invalid={!!nameError} aria-describedby={nameError ? `${key}-name-error` : undefined} value={naming} autoComplete="off" spellCheck={false}
        readOnly={snap.value === 'renaming'} onChange={e => m.send({type: 'TYPE', value: e.target.value})} onBlur={() => void commitName()}
        onKeyDown={e => {if (e.key === 'Enter') {e.preventDefault(); void commitName();} else if (e.key === 'Escape' && snap.value === 'naming') {e.preventDefault(); e.stopPropagation(); m.send({type: 'CANCEL'});}}}/>
        : <span className="pf-editor-name"><strong onClick={renamable ? startRename : undefined} data-renamable={renamable || undefined}>{profileName}</strong>{dynamic && <DynamicBadge/>}{renamable && <button type="button" className="reset-icon pf-rename" aria-label="Rename" disabled={saving} onClick={startRename}><Glyph name="edit" size={14}/></button>}</span>}</>}
    summary={nameError ? <div className="pf-editor-summary pf-name-error" id={`${key}-name-error`} role="alert">{nameError}</div> : <div className="pf-editor-summary">{type === 'process' ? autoPreferences(source) : profileSummary(type, values)}{draft?.staged && <><Sep/>Includes project changes</>}</div>}
    controls={<>{type === 'machine' && source && <NozzleSize printerId={source.id} disabled={saving || dirty || !ready}/>}{type === 'machine' && source && <BuildPlateField printerId={source.id} disabled={saving || !ready}/>}{(dirty || saving) && (newName !== null ? <span className="pf-save-as"><input className="text" autoFocus aria-label="New profile name" value={newName} disabled={saving}
          onChange={e => {setNewName(e.target.value); setError(null);}} onKeyDown={e => {if (e.key === 'Enter') {e.preventDefault(); void save(newName);} else if (e.key === 'Escape') {e.preventDefault(); e.stopPropagation(); setNewName(null);}}}/>
          <button type="button" className="btn-ghost" disabled={saving} onClick={() => setNewName(null)}>Cancel</button>
          <button type="button" className="btn primary" disabled={saving || !newName.trim()} onClick={() => void save(newName)}>{saving ? 'Saving…' : 'Create'}</button></span>
        : conflicted ? <span className="pf-save-conflict" role="status">Changed elsewhere</span>
        : <SaveMenu name={profileName} busy={saving} disabled={saving || !ready} targets={{profile: () => void save(),
          ...(type === 'filament' && Object.keys(changes).some(isCalibrationKey) ? {forPrinter: {model: printerModel, run: () => void saveForPrinter()}} : {}),
          asNew: () => setNewName(profileName.startsWith('My ') ? `${profileName} copy` : `My ${profileName}`)}}/>)}
        {dirty && !foldActions && <button type="button" className="btn" disabled={saving} onClick={revert}>Revert</button>}
        {onDuplicate && !foldActions && <button type="button" className="btn" disabled={saving || dirty || !ready} onClick={onDuplicate}>Duplicate</button>}
        <MenuButton label="Profile actions" className="icon-btn" items={[...(renamable ? [{label: 'Rename', disabled: saving, action: startRename}] : []), ...(type === 'process' && source ? [dynamic ? {label: 'Make regular', disabled: saving, action: () => void makeRegular()} : {label: 'Make dynamic', disabled: saving, action: makeDynamic}] : []), ...(foldActions && dirty ? [{label: 'Revert', disabled: saving, action: revert}] : []), ...(foldActions && onDuplicate ? [{label: 'Duplicate', disabled: saving || dirty || !ready, action: onDuplicate}] : []), {label: type === 'filament' ? 'Use in slot 1' : 'Use in project', disabled:saving || dirty, action:()=>requestPreset(type,0,key && source!.id)}, {label:'Export',output:true,action:()=>void import('./library-io').then(m=>m.exportProfiles({kind:'preset',targets:[{kind:type==='machine'?'printer':type,id:source!.id}]})).catch(e=>toast('profileeditor_failed', { detail: e instanceof Error?e.message:String(e) }))}, {label: 'Reset to base settings', disabled: saving || !Object.keys(ownValues(type, source)).length, action: resetAllToBase}, 'sep', {label:'Delete',disabled:saving,action:onRemove}]}>⋯</MenuButton>{onClose && <button type="button" className="icon-btn pf-editor-close" aria-label="Close" onClick={onClose}><Glyph name="close" size={14}/></button>}</>}>
    {snap.context.updated && snap.value !== 'conflict' && <div className="pf-editor-note" role="status" data-updated-elsewhere>Updated in another tab or device: {movesText(snap.context.updated)}.
      <button type="button" className="btn-ghost" onClick={() => m.send({type: 'SEEN'})}>OK</button></div>}
    {snap.value === 'conflict' && <div className="pf-editor-error" role="alert">Changed elsewhere: {profileName}{snap.context.conflict?.length ? `: ${movesText(snap.context.conflict)}` : ''} (saved in another tab or device while you were editing it).
      <span className="pf-conflict-actions"><button type="button" className="btn" onClick={() => {update(key, rebasedProfileDraft(latest.current, recordOf(getState(), type, name), resolvePreset(getState(), type, name).config)); m.send({type: 'KEEP'});}}>Keep my edits on the newer version</button>
      <button type="button" className="btn-ghost" onClick={() => {update(key, null); m.send({type: 'DISCARD'});}}>Discard my edits</button></span></div>}
    <fieldset ref={fieldsRef} className="pf-editor-fields" disabled={saving} aria-busy={saving}><SettingsTab type={type} hidePreset editor={editor} auxiliary={!source ? undefined : type === 'process' ? (dynamic ? {label: '✦ Dynamic', modified: scalesDirty || speedDirty, content: <DynamicTab config={shown} speedModel={speedModel} speeds={speedResolution.details} nozzleSpeeds={speedResolution.nozzles?.map((n) => ({ label: `${extruderLabel(n.extruder, speedNozzleCount, speedVendor === 'BBL')} ${n.diameter} mm`, speeds: n.speeds }))} missingReference={speedResolution.missingReference} slowerNote={mixedFlow} onSpeedModel={setSpeedModel} onUseMultiplier={useMultiplier} rows={scales} nozzle={nozzle} height={height} minHeight={minHeight} maxHeight={maxHeight} readOnly={saving} onChange={setScales} fromPrinter={{name: printerName, values: fromPrinter, printer: printer as PrinterRecord | undefined}}/>} : undefined) : type === 'filament' ? {label:'Printers',initial:scopeSnap.context.returned,content:<FilamentPrinters filament={name} dirty={dirty}/>}
      : [{label: 'Print overrides', modified: overridesDirty, content: <PrintOverrides value={overrides} defaults={overrideDefaults} printer={source as PrinterRecord | undefined} readOnly={saving} onChange={setOverrides}/>}, ...(connections ? [{label:'Connection',content:<Suspense fallback={null}><ConnectionEditor printer={source.id} name={profileName}/></Suspense>}] : [])]}/></fieldset>
  </DetailPane>;
}

export function DetailPane({label, identity, summary, controls, error, children}: {label: string; identity: ReactNode; summary: ReactNode; controls: ReactNode; error?: string | null; children: ReactNode}) {
  return <section className="pf-full-editor" aria-label={label}>
    <div className="pf-editor-heading"><div className="pf-editor-identity">{identity}{summary}</div><div className="pf-editor-controls">{controls}</div></div>
    {error && <div className="pf-editor-error" role="alert">{error}</div>}
    {children}
  </section>;
}
const autoPreferences = (p?: ProfileRecord) => { const h = (p as ProcessRecord | undefined)?.preferredHeight; return h ? `Prefers ${h.toFixed(2)} mm` : ''; };

function PrinterValuesEditor({filament, side, profileName, onClose}: {filament: string; side: PrinterSide; profileName: string; onClose?(): void}) {
  const scope = filamentScope(filament), snap = useMachine(scope), tuning = usePrinterTuning(filament, side);
  const sidePrinter = useStore((st) => st.records.printers.find((p) => p.id === side.printer));
  const saving = snap.value === 'saving';
  return <DetailPane label={`Edit ${profileName} for ${side.label}`} error={snap.value === 'failed' ? snap.context.error : null}
    identity={<span className="pf-editor-name"><strong>{profileName}</strong></span>}
    summary={<div className="pf-editor-summary">Values for {side.label}</div>}
    controls={<>{tuning.dirty && <><button type="button" className="btn primary" disabled={saving} onClick={tuning.save}>{saving ? 'Saving…' : `Save for ${side.label}`}</button>
      <button type="button" className="btn" disabled={saving} onClick={() => {tuning.revert(); scope.send({type: 'DISMISS'});}}>Revert</button></>}
      <button type="button" className="btn" disabled={saving || tuning.dirty} title={tuning.dirty ? 'Save or revert first' : undefined} onClick={() => scope.send({type: 'BACK', dirty: tuning.dirty})}>Back to {profileName}</button>
      {onClose && <button type="button" className="icon-btn pf-editor-close" aria-label="Close" onClick={onClose}><Glyph name="close" size={14}/></button>}</>}>
    {tuning.ready ? <div className="pf-editor-fields" aria-busy={saving}><OverrideList key={`${side.printer}#${side.extruder}`} tab="filament" printer={sidePrinter} value={tuning.rows} defaults={tuning.defaults} fixed={tuning.fixed} allow={isCalibrationKey} readOnly={saving} onChange={tuning.setRows}/></div>
      : <div className="pf-editor-summary" role="status">Loading {side.label}'s values…</div>}
  </DetailPane>;
}
