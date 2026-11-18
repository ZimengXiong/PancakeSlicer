import { useEffect } from 'react';
import type { Config, ConfigValue, PresetType } from '../api/types';
import { getState, setState, subscribeStore, useStore, type State } from '../state/store';
import { lineRead, listRead, loadedLine, loadedList, tuningFor } from '../catalog/read';
import type { CatalogEntry, CatalogPrinter, CatalogVariant } from '../catalog/types';
import type { LayerValues, ProfileAsk, ResolvedLayer, ResolvedNozzleSpeeds, ResolveRequest, ResolveStatus } from '../flow/resolved';
import { OPTIONS } from '../options';
import { jobValue, normalizeConfigValue, toolValue } from '../config-values';
import { getRecord, recordKey, type FilamentRecord, type LibraryRecords, type PrinterRecord, type ProcessRecord } from './records';
import { slotFilaments } from './slots';
import { plateSlots } from '../printers/slot-check';

export type ProfileRecord = PrinterRecord | FilamentRecord | ProcessRecord;
const kindOf = (type: PresetType) => type === 'machine' ? 'printer' : type === 'process' ? 'process' : 'filament';
export const recordOf = (s: State, type: PresetType, id: string) => getRecord(s.records, recordKey(kindOf(type), id)) as ProfileRecord | undefined;

export interface ProfileValues {
  state: 'resolving' | 'ready' | 'failed';
  values: Config;
  layers: Record<string, LayerValues>;
  status?: ResolveStatus;
  error?: string;
  nozzles?: ResolvedNozzleSpeeds[];
}
const EMPTY: Config = {};
const NONE: Record<string, LayerValues> = {};
const memo = new WeakMap<object, Map<string, ProfileValues>>();
function once(view: object, key: string, make: () => ProfileValues): ProfileValues {
  let m = memo.get(view); if (!m) memo.set(view, (m = new Map()));
  let v = m.get(key); if (!v) m.set(key, (v = make()));
  return v;
}
const inputMemo = new WeakMap<object, Map<string, { inputs: unknown[]; value: ProfileValues }>>();
function onceFor(view: object, key: string, inputs: unknown[], make: () => ProfileValues): ProfileValues {
  let m = inputMemo.get(view); if (!m) inputMemo.set(view, (m = new Map()));
  const hit = m.get(key);
  if (hit && hit.inputs.every((v, i) => v === inputs[i])) return hit.value;
  const value = make(); m.set(key, { inputs, value });
  return value;
}
const askKey = (a: ProfileAsk) => JSON.stringify([a.type, a.id, a.printer ?? null, a.extruder ?? null]);
const sameAsk = (a: ProfileAsk, b: ProfileAsk) => a.type === b.type && a.id === b.id && a.printer === b.printer && a.extruder === b.extruder;

export function selectionValues(s: State, type: PresetType, slot = 0): ProfileValues {
  const inputs = [s.dirty, s.filamentEdits, s.projectPresets, s.processVariant, slotFilaments(s)[slot]];
  return onceFor(s.resolved, `${type}:${slot}`, inputs, () => selection(s, type, slot));
}
function selection(s: State, type: PresetType, slot: number): ProfileValues {
  const { state, answer, answeredFor: was, error } = s.resolved, p = !answer ? undefined : type === 'machine' ? answer.printer : type === 'process' ? answer.print : answer.filaments[slot];
  const same = !!was && (type === 'machine' ? was.printer === s.printer : type === 'process' ? was.print === s.process : was.filaments[slot] === slotFilaments(s)[slot]);
  const capped = state === 'ready' || !!was && JSON.stringify([was.filaments, was.usedSlots]) === JSON.stringify([slotFilaments(s), plateSlots(s)]);
  const layers = p?.layers ?? NONE, pending = p && same ? pendingEdits(s, type, slot, was, layers) : null;
  return { state: state === 'failed' ? 'failed' : p && (state === 'ready' || state === 'resolving' && same) ? 'ready' : 'resolving', values: pending ? { ...p!.values, ...pending } : p?.values ?? EMPTY, layers, status: p?.status, error: error ?? undefined, nozzles: capped ? p?.nozzles : undefined };
}
function pendingEdits(s: State, type: PresetType, slot: number, was: ResolveRequest, layers: Record<string, LayerValues>): Config | null {
  const now: Config = type === 'machine' ? { ...s.projectPresets?.machine, ...s.dirty.machine } : type === 'process' ? { ...s.projectPresets?.process, ...s.dirty.process }
    : { ...s.projectPresets?.filaments[slot], ...s.filamentEdits[slotFilaments(s)[slot]] };
  const sent = (type === 'machine' ? was.project.machine : type === 'process' ? was.project.process : was.project.filaments?.[slot]) ?? EMPTY;
  const out: Config = {};
  for (const [k, v] of Object.entries(now))
    if (k !== 'layer_height' && OPTIONS[k] && (sent[k] === undefined || toolValue(k, jobValue(k, v)) !== toolValue(k, sent[k]))) out[k] = normalizeConfigValue(k, v);
  for (const k of Object.keys(sent)) if (!(k in now)) { const v = valueBelow(layers[k], 'project'); if (v !== undefined) out[k] = v; }
  const height = s.processVariant?.layerHeight ?? null;
  if (type === 'process' && height !== null && height !== was.layerHeight) out.layer_height = normalizeConfigValue('layer_height', String(height));
  return Object.keys(out).length ? out : null;
}
export function askedValues(s: State, ask: ProfileAsk): ProfileValues {
  return once(s.resolved, askKey(ask), () => asked(s, ask));
}
function asked(s: State, ask: ProfileAsk): ProfileValues {
  const { state, answer, error } = s.resolved, p = answer?.profiles?.find((x) => sameAsk(x, ask));
  return { state: state === 'failed' ? 'failed' : p && (state === 'ready' || state === 'resolving') ? 'ready' : 'resolving', values: p?.values ?? EMPTY, layers: p?.layers ?? NONE, status: p?.status, error: error ?? undefined, nozzles: p?.nozzles };
}

export function useAskedValues(ask: ProfileAsk | null): ProfileValues | null {
  useAsks(ask ? [ask] : []);
  return useStore((s) => ask ? askedValues(s, ask) : null);
}
export function useAsks(asks: ProfileAsk[]): void {
  const key = JSON.stringify(asks.map(askKey));
  useEffect(() => {
    if (!asks.length) return;
    setState((s) => ({ resolveAsks: [...s.resolveAsks, ...asks] }));
    return () => setState((s) => {
      const left = [...s.resolveAsks];
      for (const ask of asks) { const i = left.findIndex((a) => sameAsk(a, ask)); if (i >= 0) left.splice(i, 1); }
      return { resolveAsks: left };
    });
  }, [key]); // eslint-disable-line react-hooks/exhaustive-deps
}
export const distinctAsks = (asks: ProfileAsk[]) => asks.filter((a, i) => asks.findIndex((b) => sameAsk(a, b)) === i);
export const currentAsks = () => distinctAsks(getState().resolveAsks);

export interface ProfileInfo {
  type: PresetType; id: string; name: string; record?: ProfileRecord; archived: boolean;
  vendor?: string; model?: string; variants?: string[]; printer?: CatalogPrinter;
  product?: string; entry?: CatalogEntry;
}
const infos = new WeakMap<LibraryRecords, Map<string, ProfileInfo>>();
export function profileInfo(s: State, type: PresetType, id: string): ProfileInfo {
  let m = infos.get(s.records); if (!m) infos.set(s.records, (m = new Map()));
  const key = `${s.catalogReads}:${type}:${id}`;
  let v = m.get(key); if (!v) m.set(key, (v = info(s, type, id)));
  return v;
}
function info(s: State, type: PresetType, id: string): ProfileInfo {
  const record = recordOf(s, type, id), base = { type, id, name: record?.name ?? '', record, archived: !!record?.archived };
  if (!record) return base;
  if (type === 'machine') {
    const p = record as PrinterRecord;
    return { ...base, vendor: p.vendor, model: p.model, variants: p.nozzles.map((n) => n.variant), printer: loadedLine('printers', p.model) };
  }
  const product = type === 'filament' ? (record as FilamentRecord).product : (record as ProcessRecord).product;
  const list = loadedList(type === 'filament' ? 'filaments' : 'prints');
  return { ...base, product, entry: product ? list?.entries.find((e) => e.id === product) : undefined, vendor: product ? list?.entries.find((e) => e.id === product)?.brand : undefined };
}
export function variantOf(s: State, printerId: string, extruder = 0): { id: string; variant?: CatalogVariant } {
  const info = profileInfo(s, 'machine', printerId), id = info.variants?.[extruder] ?? '';
  return { id, variant: info.printer?.variants.find((v) => v.id === id) };
}

let watching = false;
export function watchCatalogLines(): void {
  if (watching) return;
  watching = true;
  const seen = new Set<object>();
  const track = (m: { subscribe(l: (snap: { value: string }) => void): () => void }) => {
    if (seen.has(m)) return;
    seen.add(m);
    m.subscribe((snap) => { if (snap.value === 'ready') setState((st) => ({ catalogReads: st.catalogReads + 1 })); });
  };
  track(listRead('filaments')); track(listRead('prints'));
  let last: LibraryRecords | null = null;
  const check = () => {
    const doc = getState().records;
    if (doc === last) return;
    last = doc;
    for (const p of doc.printers) if (!p.archived && p.model) track(lineRead('printers', p.model));
    for (const f of doc.filaments) if (!f.archived && f.product) track(lineRead('filaments', f.product));
  };
  subscribeStore(check); check();
}

export interface ProfileBase {
  name: string; vendor: string; model?: string; native_support_capable?: boolean;
  bed_model?: string; bed_texture?: string; default_bed_type?: string; not_support_bed_type?: string[]; default_materials?: string[];
  filament_id?: string;
}
const text = (v: unknown) => typeof v === 'string' ? v : undefined;
const list = (v: unknown) => Array.isArray(v) ? v.map(String) : typeof v === 'string' && v ? v.split(';') : undefined;
export interface ResolvedProfile {
  key: string; name?: string; record?: ProfileRecord; base?: ProfileBase; config: Config;
  state: ProfileValues['state']; layers: Record<string, LayerValues>; error?: string;
  nozzles?: ResolvedNozzleSpeeds[];
}
const resolvedMemo = new WeakMap<object, Map<string, ResolvedProfile>>();
export function resolvedProfile(s: State, type: PresetType, key: string): ResolvedProfile {
  let m = resolvedMemo.get(s.resolved); if (!m) resolvedMemo.set(s.resolved, (m = new Map()));
  const id = `${s.catalogReads}:${type}:${key}`;
  let v = m.get(id);
  if (!v || v.record !== recordOf(s, type, key)) m.set(id, (v = resolveNow(s, type, key)));
  return v;
}
function resolveNow(s: State, type: PresetType, key: string): ResolvedProfile {
  const info = profileInfo(s, type, key);
  const slot = type === 'filament' ? slotFilaments(s).indexOf(key) : 0;
  const selected = type === 'machine' ? key === s.printer : type === 'process' ? key === s.process : slot >= 0;
  const values = selected ? selectionValues(s, type, Math.max(slot, 0)) : askedValues(s, { type, id: key });
  const variant = type === 'machine' ? info.printer?.variants.find((v) => v.id === info.variants?.[0]) : undefined;
  const m = info.printer?.model ?? {};
  const onVariant = type === 'filament' && info.product ? loadedLine('filaments', info.product) : undefined;
  const base: ProfileBase | undefined = !info.record ? undefined : type === 'machine'
    ? { name: info.variants?.[0] ?? '', vendor: info.vendor ?? '', model: info.model, native_support_capable: !!variant?.source.native_support_capable,
      bed_model: text(m.bed_model), bed_texture: text(m.bed_texture), default_bed_type: text(m.default_bed_type), not_support_bed_type: list(m.not_support_bed_type), default_materials: list(m.default_materials) }
    : { name: info.product ?? 'Auto', vendor: info.vendor ?? '', ...(onVariant ? { filament_id: tuningFor(onVariant, variantOf(s, s.printer).id)?.source.filament_id } : {}) };
  const error = !info.record ? 'This profile was removed from your library.' : values.status && !values.status.ok ? values.status.message : values.error;
  return { key, name: info.name, record: info.record, base, config: values.values, state: values.state, layers: values.layers, nozzles: values.nozzles, ...(error ? { error } : {}) };
}

export const nozzleCount = (s: State, printerId = s.printer) => Math.max(1, s.records.printers.find((p) => p.id === printerId)?.nozzles.length ?? 1);


export function ownValues(type: PresetType, record: ProfileRecord | undefined): Config {
  if (!record) return EMPTY;
  return (type === 'machine' ? (record as PrinterRecord).machine : type === 'filament' ? (record as FilamentRecord).settings : (record as ProcessRecord).values) ?? EMPTY;
}
export function withOwnValues<R extends ProfileRecord>(type: PresetType, record: R, own: Config): R {
  const values = Object.keys(own).length ? { ...own } : undefined;
  return { ...record, ...(type === 'machine' ? { machine: values } : type === 'filament' ? { settings: values } : { values }) };
}
export const recordKind = (type: PresetType) => type === 'machine' ? 'printer' as const : type === 'process' ? 'process' as const : 'filament' as const;

export function objectValues(s: State, objectId: number | string): ProfileValues {
  return once(s.resolved, `object:${objectId}`, () => {
    const { state, answer, error } = s.resolved, o = answer?.objects?.find((x) => x.id === String(objectId));
    if (!o) {
      const own = s.objects.find((x) => String(x.id) === String(objectId))?.settings;
      return own && Object.keys(own).length ? { state: state === 'failed' ? 'failed' : 'resolving', values: EMPTY, layers: NONE, error: error ?? undefined } : selectionValues(s, 'process');
    }
    return { state: state === 'failed' ? 'failed' : state === 'ready' ? 'ready' : 'resolving', values: o.values, layers: o.layers, status: o.status, error: error ?? undefined };
  });
}

const LAYERS: ResolvedLayer[] = ['default', 'base', 'tuning', 'record', 'auto', 'printer', 'calibration', 'project', 'object'];
export function valueBelow(layers: LayerValues | undefined, layer: ResolvedLayer): ConfigValue | undefined {
  if (!layers) return undefined;
  for (let i = LAYERS.indexOf(layer) - 1; i >= 0; i--) if (layers[LAYERS[i]] !== undefined) return layers[LAYERS[i]];
  return undefined;
}
export function belowProject(layers: Record<string, LayerValues>): Config {
  const out: Config = {};
  for (const [key, l] of Object.entries(layers)) { const v = valueBelow(l, 'project'); if (v !== undefined) out[key] = v; }
  return out;
}
