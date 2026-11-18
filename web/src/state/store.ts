import { observePreference, preferenceText } from '../saved-preferences';
import { emptyLibrary, type LibraryRecords } from '../profiles/records';
import { timeInteraction } from '../interaction-metrics';
import { app } from '../flow/app';
import { tools } from '../flow/tools';
import { useSyncExternalStore } from 'react';
import { plateSignature } from '../slice-input';
import type * as THREE from 'three';
import { T3 } from '../three-runtime';
import type { Api } from '../api/client';
import type { Config, ConfigValue, JobStatus, PresetType } from '../api/types';
import type { ProjectPlate } from '../plater/threemf';
import { OPTIONS, type OptionMode } from '../options';
import { undo, undoable } from './history';
import { selectChanges } from './changes';
import type { PaintKind, Painting } from '../plater/paint';
import type { EngineWarning } from '../engine-warnings';
import { NOTICES, noticeText, type NoticeCode, type NoticeKind, type NoticeParams } from '../notice-codes';
import { type ProjectProfileReferences, type ProfileRecovery } from '../profiles/ownership';
import { planPrinterSwitch } from '../profiles/material-library';
import { findProfile,profileKey } from '../profiles/identity';
import type { PrinterConnections } from '../printers/connections';
export { PRESET_META } from '../profiles/model';
import { resolvedProfile, selectionValues, type ProfileBase } from '../profiles/view';
import { slotFilaments } from '../profiles/slots';
export { selectCompatibility } from '../profiles/compatibility';


export interface Instance {
  id: number;
  x: number;
  y: number;
  rotZ: number;
  native?: { json: Record<string, unknown>; x: number; y: number; z: number; rotZ: number };
  z?: number;
  printable?: boolean;
}

export type VolumeType = 'part' | 'negative' | 'modifier' | 'support_blocker' | 'support_enforcer' | 'seam';
export type SeamMode = 'center' | 'left' | 'right' | 'enforced' | 'blocked' | 'neutral';

export interface Volume {
  seamMode?: SeamMode;
  id: number;
  name: string;
  type: VolumeType;
  geometry: THREE.BufferGeometry;
  matrix: number[];
  extruder: number;
  native?: number;
  settings: Config;
  paint?: Partial<Record<PaintKind, Painting>>;
  text?: TextSpec;
  svg?: SvgSpec;
}

export interface SvgSpec {
  svg: string;
  name: string;
  depth: number;
  size?: number;
  surface?: TextSpec['surface'];
}

export interface TextSpec {
  text: string;
  font: string;
  size: number;
  depth: number;
  bold: boolean;
  italic: boolean;
  lineSpacing: number;
  charGap: number;
  align: 'left' | 'center' | 'right';
  surface?: { at: [number, number, number]; normal: [number, number, number] };
}

export interface NativeObject {
  entry: Record<string, unknown>;
  frame: number[];
  base: { rotX: number; rotY: number; scale: [number, number, number] };
  main: number;
  files: Record<string, Blob>;
  layerProfile?: number[];
}

export interface LayerRange { id: number; z: [number, number]; layer_height: number; settings: Config }

export interface Mix { components: number[]; ratios: number[]; gradient: 0 | 1 | null; perPart: boolean }

export interface PlateObject {
  id: number;
  name: string;
  geometry: THREE.BufferGeometry;
  rotX: number;
  rotY: number;
  scale: [number, number, number];
  extruder: number;
  instances: Instance[];
  source: File;
  sourceUrl?: string;
  simplification?: { trianglesBefore: number; trianglesAfter: number; maxErrorMm: number; kept: 'simplified' | 'original' };
  settings: Config;
  volumes?: Volume[];
  native?: NativeObject;
  ranges?: LayerRange[];
  paint?: Partial<Record<PaintKind, Painting>>;
  brimEars?: { p: [number, number, number]; r: number }[];
  text?: TextSpec;
  svg?: SvgSpec;
  units?: 'inch' | 'meter';
  layerProfile?: number[];
  project?: { buildIndex?: number; plateIndex?: number; plateOrigin?: [number, number]; file: string; placeholder: boolean; missing?: boolean; origin?: [number, number, number];
    saved?: { object: number; instance: number; instanceId: number; extruder: number; rotZ: number; item?: number[] };
    paintable?: boolean; paint?: Partial<Record<'support' | 'seam' | 'fuzzy' | 'color', Record<string, string>>>; partTris?: number[] };
}

export interface LayerEvent {
  z: number;
  type: 'pause' | 'custom' | 'template' | 'colour' | 'change';
  text?: string;
}

export interface HeadInfo {
  id: string;
  name: string;
  calibrated: boolean;
  user?: boolean;
}

export type PresetUx = { kind: 'export' } | { kind: 'import' };
export interface LibraryTarget { type: PresetType; name?: string; edit?: boolean; add?: boolean; compare?: string;
  stage?: import('../profiles/project-draft').ProfileSave; at: number }

export interface SlideOutTarget { type: PresetType; key: string; setting?: string; at: number }
export type MainTab = 'projects' | 'prepare' | 'preview' | 'device' | 'library';
export type CalibMenu = 'temperature' | 'max-flowrate' | 'pressure-advance' | 'flow-ratio' | 'retraction' | 'cornering' | 'input-shaping-freq' | 'input-shaping-damp' | 'vfa' | 'nonplanar';
export type Gizmo = 'none' | 'move' | 'rotate' | 'scale' | 'flatten' | 'cut' | 'slant' | 'measure' | 'layers' | 'brimears' | 'boolean' | 'text' | 'svg' | PaintKind;

export interface JobView {
  id?: string;
  state: 'idle' | 'submitting' | 'running' | 'done' | 'failed' | 'canceled';
  pct: number;
  stage: string;
  queuePosition?: number;
  error?: string;
  errorCode?: string;
  status?: JobStatus;
  stale?: boolean;
  plateSignature?: string;
  warnings?: EngineWarning[];
  calib?: string;
  calibLog?: string[];
  exported?: string;
}

export interface SlantEdit {
  z: number;
  tilt: number;
  dir: number;
  dirAuto?: boolean;
  auto: boolean;
  limit?: number;
}

export interface CutPlane {
  z: number;
  rotX: number;
  rotY: number;
  keepUpper: boolean;
  keepLower: boolean;
  placeLower: boolean;
  flipUpper?: boolean;
  toParts?: boolean;
  mode?: 'planar' | 'dovetail';
  connectors?: CutConnector[];
  connector?: CutConnectorAttribs;
  selectedConnectors?: number[];
  editing?: boolean;
  groove?: CutGroove;
}

export interface CutConnector { pos: [number, number]; a: CutConnectorAttribs }

export interface CutConnectorAttribs {
  type: 'plug' | 'dowel' | 'snap';
  style: 'prism' | 'frustum';
  shape: 'triangle' | 'square' | 'hexagon' | 'circle';
  depth: number; depthTolerance: number; size: number; sizeTolerance: number; angle: number;
  bulge: number; space: number;
}

export interface CutGroove { depth: number; depthTolerance: number; width: number; widthTolerance: number; flapsAngle: number; angle: number; count: number; gap: number }

export interface State {
  api: Api | null;
  cut: CutPlane | null;
  slant: SlantEdit | null;
  loading: string | null;
  records: LibraryRecords;
  profileLibraryLoad: {state: 'loading'} | {state: 'ready'} | {state: 'failed'; error: string};
  libraryHealth: import('../persist/profile-library').LibraryHealth;
  resolved: import('../flow/resolved').ResolvedView;
  resolveAsks: import('../flow/resolved').ProfileAsk[];
  catalogReads: number;
  profileLibraryRevision: number;
  projectProfileContexts: Record<string,{projectPresets:State['projectPresets'];profileSnapshots:ProjectProfileReferences|null;process:string;dirty:State['dirty']}>;
  profileSnapshots: ProjectProfileReferences | null;
  profileRecoveries: ProfileRecovery[];
  printer: string;
  process: string;
  libraryDrafts: Record<string, import('../profiles/ProfileEditor').ProfileDraft>;
  dirty: Record<'machine' | 'process', Config>;
  filamentEdits: Record<string, Config>;
  projectPresets: { machine: Config; process: Config; filaments: Config[] } | null;
  projectSlotValues: (Config | null)[] | null;
  projectSlots: { filaments: string[]; colours: (string | undefined)[]; mixes: Record<number, unknown> } | null;
  uiMode: OptionMode;
  developerMode: boolean;
  mixedTemperatures: boolean;
  tab: MainTab;
  dialog: { type: 'machine' | 'filament'; slot: number } | null;
  objects: PlateObject[];
  selected: Selected | null;
  gizmo: Gizmo;
  uniformScale: boolean;
  contextMenu: { x: number; y: number; object?: number; instance?: number; volume?: number } | null;
  renaming: number | null;
  meshErrors: Record<number, number>;
  clipboard: PlateObject[];
  processVariant: { layerHeight: number } | null;
  projectMaterials: { colours: string[]; matrix: number[] | null; multiplier: number | null; builds?: { object: number; instance: number }[] } | null;
  flushDialog: boolean;
  appDialog: 'about' | 'preferences' | 'shortcuts' | 'send' | 'setup' | 'plate' | 'clone' | 'simplify' | 'objectTable' | 'createPrinter' | 'mixed' | 'what' | null;
  calibDialog: CalibMenu | null;
  printerConnections: PrinterConnections;
  events: LayerEvent[];
  projectPlates: (ProjectPlate & { objects: PlateObject[]; events?: LayerEvent[] })[];
  plateIndex: number;
  project: File | null;
  projectName: string | null;
  job: JobView;
  noticeLog: NoticeEntry[];
  toast: string | null;
  toastSeq: number;
  toastAction: { label: string; run: () => void; msg?: string } | null;
  toastKind: NoticeKind | null;
  toastStep: string | null;
  toastMs: number | null;
  recommendedStyle: Record<number, 'standard'>;
  heads: HeadInfo[] | null;
  presetUx: PresetUx | null;
  library: LibraryTarget;
  slideOut: SlideOutTarget | null;
  device: { printer: string; at: number };
  settingsFocus: { type: PresetType; page: string; key: string; nonce: number } | null;
  sessionDialog: 'recent' | null;
  orientPrompts: Record<number, OrientPrompt>;
  orientDismissed: number[];
  wipeTowerPos: Record<number, [number, number]>;
  wipeTower: WipeTowerEstimate | null;
  orientConfirm: OrientConfirm | null;
  settingsClipboard: { kind: 'object' | 'volume'; config: Config } | null;
}

export interface OrientPrompt {
  rotation: number[];
  pose: string;
  supportArea: [number, number];
}

export interface WipeTowerEstimate { x: number; y: number; width: number; depth: number; height: number; brim_width: number }
export interface OrientSlice { time_s: number | null; support_g: number | null; floating_mm: number | null }

export interface OrientConfirm {
  id: number;
  supportOn: boolean;
  supportArea: [number, number];
  original?: OrientSlice;
  standing?: OrientSlice;
  error?: string;
}

export const IDLE_JOB: JobView = { state: 'idle', pct: 0, stage: '' };
let state: State = {
  api: null,
  orientPrompts: {},
  orientDismissed: [],
  wipeTowerPos: {},
  wipeTower: null,
  orientConfirm: null,
  settingsClipboard: null,
  cut: null,
  slant: null,
  loading: 'Connecting',
  records: emptyLibrary(),
  profileLibraryLoad: {state: 'loading'},
  libraryHealth: { problems: {}, notices: [] },
  resolved: { state: 'idle', answer: null, answeredFor: null, error: null },
  resolveAsks: [],
  catalogReads: 0,
  profileLibraryRevision: 0,
  projectProfileContexts: {},
  profileSnapshots: null,
  profileRecoveries: [],
  printer: '',
  process: '',
  libraryDrafts: {},
  dirty: { machine: {}, process: {} },
  filamentEdits: {},
  projectPresets: null,
 
  projectSlotValues: null,
  projectSlots: null,
  uiMode: 'simple',
  developerMode: false,
  mixedTemperatures: false,
  tab: 'prepare',
  dialog: null,
  objects: [],
  selected: null,
  gizmo: 'none',
  uniformScale: true,
  contextMenu: null,
  renaming: null,
  meshErrors: {},
  clipboard: [],
  
  processVariant: null,
  projectMaterials: null,
  flushDialog: false,
  appDialog: null,
  calibDialog: null,
  printerConnections: { enabled: null, live: false, connections: [], status: {}, transfers: [] },
  events: [],
  projectPlates: [],
  plateIndex: 1,
  project: null,
  projectName: null,
  job: IDLE_JOB,
  noticeLog: [],
  toast: null,
  toastSeq: 0,
  toastAction: null,
  toastKind: null,
  toastStep: null,
  toastMs: null,
  recommendedStyle: {},
  heads: null,
  presetUx: null,
  library: { type: 'machine', at: 0 },
  slideOut: null,
  device: { printer: '', at: 0 },
  settingsFocus: null,
  sessionDialog: null,
};

const listeners = new Set<() => void>();
export const getState = () => state;
export function setState(patch: Partial<State> | ((s: State) => Partial<State>)) {
  const finishTiming = timeInteraction('store');
  let next = { ...state, ...(typeof patch === 'function' ? patch(state) : patch) };
  const selected = next.selected;
  if (selected && !next.objects.some((o) => o.id === selected.object && o.instances.some((i) => i.id === selected.instance))) {
    next.selected = null;
    next.gizmo = 'none';
  } else if (selected?.volume !== undefined && !next.objects.find((o) => o.id === selected.object)?.volumes?.some((v) => v.id === selected.volume)) {
    next.selected = { ...selected, volume: undefined, allInstances: true };
  }
  if (next.selected !== tools.get().context.selected || next.gizmo !== tools.get().context.gizmo)
    tools.send({ type: 'SYNC', selected: next.selected, gizmo: next.gizmo });
  if (next.job.plateSignature && !next.job.stale && next.job.plateSignature !== plateSignature(next))
    next.job = { ...next.job, stale: true };
  state = next;
  if ((app.get().value === 'empty' && next.objects.length) || (app.get().value === 'ready' && !next.objects.length))
    app.send({ type: 'DOCUMENT', hasObjects: next.objects.length > 0 });
  listeners.forEach((l) => l());
  finishTiming();
}
const subscribe = (l: () => void) => {
  listeners.add(l);
  return () => listeners.delete(l);
};
export const subscribeStore = subscribe;
export function useStore<T>(sel: (s: State) => T): T {
  return useSyncExternalStore(subscribe, () => sel(state));
}

export const effectiveMode = (s: State): OptionMode => s.developerMode ? 'developer' : s.uiMode;

let nextId = 1;
export const newId = () => nextId++;
export const reserveIds = (max: number) => { nextId = Math.max(nextId, max + 1); };

export const DEMO_MODELS = ['Bambu Lab A1 mini', 'Bambu Lab A1', 'Bambu Lab P1S', 'Prusa MINI', 'Prusa MINI IS', 'Prodcan3 5-axis BC table', 'Fairino FR3 cell'];
export const DEFAULT_PRINTER = 'Bambu Lab A1 mini 0.4 nozzle';


export function resolvePreset(s: State, type: PresetType, key: string) { return resolvedProfile(s, type, key); }

export function effectiveConfig(s: State, type: PresetType, slot = 0): Config {
  return selectionValues(s, type, slot).values;
}


export interface PrinterPreset extends ProfileBase { config: Config }
const printerCache = new WeakMap<ProfileBase, WeakMap<Config, PrinterPreset>>();
export function printerPreset(s: State): PrinterPreset | undefined {
  const { base } = resolvePreset(s, 'machine', s.printer);
  if (!base) return undefined;
  const config = effectiveConfig(s, 'machine');
  let configs = printerCache.get(base);
  if (!configs) printerCache.set(base, (configs = new WeakMap()));
  let preset = configs.get(config);
  if (!preset) configs.set(config, (preset = { ...base, config }));
  return preset;
}



export function selectPrinter(name: string) {
  const initial=getState();
  if(initial.profileLibraryLoad.state!=='ready')throw new Error('Wait for your profile library to finish loading before switching printers (your current setup is safe!).');
  const target=findProfile(initial,'machine',name);
  if(!target || target.archived)throw new Error('Add this printer template to your library first so you can customize and use it.');
  setState(s=>{
    const key=profileKey(target),setup=planPrinterSwitch(s,key);
    const next={...s,...setup,profileRecoveries:setup.recoveries,profileSnapshots:null,
      dirty:{machine:{},process:s.dirty.process},job:markStale(s.job)};
    return next;
  });
  void import('../profiles/printer-setups').then(m=>m.saveSelectionStrip());
}

const markStale = (job: JobView): JobView => (job.state === 'done' ? { ...job, stale: true } : job);

const EMPTY_EDITS: Config = {};
export function presetEdits(s: State, type: PresetType, slot = 0): Config {
  return type === 'filament' ? s.filamentEdits[slotFilaments(s)[slot]] ?? EMPTY_EDITS : s.dirty[type];
}

const settingName = (key: string) => `Change setting "${OPTIONS[key]?.label || key}"`;

export function setOption(type: PresetType, key: string, value: ConfigValue, slot = 0) {
  if (type === 'process' && key === 'layer_height') { const h = Number(Array.isArray(value) ? value[0] : value); if (h > 0) setLayerHeight(h); return; }
  if (type === 'process' && key.endsWith('_speed')) void import('../flow/speed-cap-notice').then((m) => m.noteSpeedCap(key, value));
  undoable(settingName(key), () => setState((s) => {
    const edits = { ...presetEdits(s, type, slot), [key]: value };
    const patch:Partial<State>={...(type==='filament' ? {filamentEdits:{...s.filamentEdits,[slotFilaments(s)[slot]]:edits}} : {dirty:{...s.dirty,[type]:edits}}),job:markStale(s.job)};
    return patch;
  }), `${type}:${slot}:${key}`);
}

export function setLayerHeight(layerHeight: number) {
  const limit = getState().resolved.answer?.print.heights?.limits.reduce<{ max: number; diameter: number } | null>((low, l) => !low || l.max < low.max ? l : low, null);
  if (limit && layerHeight > limit.max + 1e-9) {
    const kept = getState().processVariant?.layerHeight ?? Number([effectiveConfig(getState(), 'process').layer_height].flat()[0]);
    toast('maxes_out_mm_mm', { max: limit.max, diameter: limit.diameter, value: kept ? `: kept ${kept}` : '' });
    return;
  }
  undoable('Layer height', () => setState((s) => ({ processVariant: { layerHeight }, job: markStale(s.job) })), 'process:layer_height');
}

export function resetOption(type: PresetType, key: string, slot = 0) {
  if (type === 'process' && key === 'layer_height') { undoable('Layer height', () => setState((s) => ({ processVariant: null, dirty: { ...s.dirty, process: Object.fromEntries(Object.entries(s.dirty.process).filter(([k]) => k !== 'layer_height')) }, job: markStale(s.job) })), 'process:layer_height'); return; }
  undoable(`Reset setting "${OPTIONS[key]?.label || key}"`, () => setState((s) => {
    const edits = { ...presetEdits(s, type, slot) };
    delete edits[key];
    return {
      ...(type === 'filament' ? { filamentEdits: { ...s.filamentEdits, [slotFilaments(s)[slot]]: edits } }
        : { dirty: { ...s.dirty, [type]: edits } }),
      job: markStale(s.job),
    };
  }));
}

export function hasUnsavedEdits(s: State, type: PresetType, slot: number) {
  if (!selectChanges(s, type, slot).some((c) => c.scope === 'project' && c.owner.plate === undefined)) return false;
  return type !== 'filament' || !slotFilaments(s).some((f, i) => i !== slot && f === slotFilaments(s)[slot]);
}


export interface NoticeEntry { id: number; message: string; kind: NoticeKind }
export interface ToastOptions { action?: State['toastAction']; ms?: number }
type NoticeArgs<C extends NoticeCode> = NoticeParams<C> extends undefined ? [params?: undefined, options?: ToastOptions] : [params: NoticeParams<C>, options?: ToastOptions];

export function logNotice<C extends NoticeCode>(code: C, ...[params]: NoticeArgs<C>) {
  recordNotice(noticeText(code, params as NoticeParams<C>), NOTICES[code].kind);
}
function recordNotice(message: string, kind: NoticeKind) {
  setState((s) => ({ noticeLog: [...s.noticeLog.slice(-99), { id: (s.noticeLog.at(-1)?.id ?? 0) + 1, message, kind }] }));
}

export function toast<C extends NoticeCode>(code: C, ...[params, options]: NoticeArgs<C>) {
  const notice: { kind: NoticeKind; step?: string; ms?: number; interrupt?: boolean } = NOTICES[code];
  const msg = noticeText(code, params as NoticeParams<C>);
  const action = options?.action ?? null;
  recordNotice(msg, notice.kind);
  if (notice.interrupt || notice.kind === 'error' || notice.kind === 'warning' || action?.label === 'Undo')
    setState({ toast: msg, toastSeq: getState().toastSeq + 1, toastAction: action && { ...action, msg },
      toastMs: options?.ms ?? notice.ms ?? (action || notice.kind === 'error' ? 8000 : 3500), toastKind: notice.kind, toastStep: notice.step ?? null });
}

function editName(patch: object): string {
  const k = Object.keys(patch);
  return k.includes('scale') ? 'Scale' : k.some((x) => x.startsWith('rot')) ? 'Rotate' : k.some((x) => 'xyz'.includes(x)) ? 'Move'
    : k.includes('extruder') ? 'Change Filament' : k.includes('name') ? 'Rename' : 'Edit object';
}

export function setPaint(id: number, kind: PaintKind, paint: Painting | null, name: string, volume?: number) {
  const painted = <T extends { paint?: Partial<Record<PaintKind, Painting>> }>(x: T): T => ({ ...x, paint: { ...x.paint, [kind]: paint ?? undefined } });
  undoable(name, () => setState((s) => ({
    objects: s.objects.map((o) => (o.id !== id ? o : volume === undefined ? painted(o)
      : { ...o, volumes: o.volumes?.map((v) => (v.id === volume ? painted(v) : v)) })),
    job: markStale(s.job),
  })));
}

export function mapPaint(id: number, kind: PaintKind, name: string, f: (target: { geometry: THREE.BufferGeometry; paint?: Partial<Record<PaintKind, Painting>> }, scale: number) => Painting | null) {
  undoable(name, () => setState((s) => ({
    objects: s.objects.map((o) => {
      if (o.id !== id) return o;
      const scale = new T3.Matrix4().compose(new T3.Vector3(), new T3.Quaternion(), new T3.Vector3(...o.scale)).getMaxScaleOnAxis();
      const next = <T extends { geometry: THREE.BufferGeometry; paint?: Partial<Record<PaintKind, Painting>> }>(x: T): T => {
        const p = f(x, scale);
        return p === (x.paint?.[kind] ?? null) ? x : { ...x, paint: { ...x.paint, [kind]: p ?? undefined } };
      };
      return { ...next(o), volumes: o.volumes?.map((v) => (v.type === 'part' ? next(v) : v)) };
    }),
    job: markStale(s.job),
  })));
}

export function updateObject(id: number, patch: Partial<PlateObject>, name = editName(patch)) {
  undoable(name, () => setState((s) => ({ objects: s.objects.map((o) => (o.id === id ? { ...o, ...patch } : o)), job: markStale(s.job) })));
}

export function updateInstance(objectId: number, instanceId: number, patch: Partial<Instance>, name = editName(patch)) {
  undoable(name, () => setState((s) => ({
    objects: s.objects.map((o) =>
      o.id === objectId ? { ...o, instances: o.instances.map((i) => (i.id === instanceId ? { ...i, ...patch } : i)) } : o,
    ),
    job: markStale(s.job),
  })));
}

export type Selected = { object: number; instance: number; allInstances?: boolean; all?: boolean; picked?: string[]; volume?: number; range?: number };

export function isSelected(sel: Selected | null, object: number, instance: number) {
  if (!sel) return false;
  if (sel.all) return !sel.picked || sel.picked.includes(`${object}:${instance}`);
  return sel.object === object && (!!sel.allInstances || sel.instance === instance);
}

export const selectedObjectIds = (s: State) => s.objects.filter((o) => o.instances.some((i) => isSelected(s.selected, o.id, i.id))).map((o) => o.id);

export function selectionOf(s: State, keys: string[]): Selected | null {
  const items = s.objects.flatMap((o) => o.instances.filter((i) => keys.includes(`${o.id}:${i.id}`)).map((i) => ({ o, i })));
  if (!items.length) return null;
  const [{ o, i }] = items;
  if (items.every((x) => x.o === o)) {
    if (items.length === 1) return { object: o.id, instance: i.id };
    if (items.length === o.instances.length) return { object: o.id, instance: i.id, allInstances: true };
  }
  return { object: o.id, instance: i.id, all: true, picked: items.map((x) => `${x.o.id}:${x.i.id}`) };
}

export function toggleSelection(object: number, instances: number[]) {
  const s = state, keys = instances.map((i) => `${object}:${i}`);
  const now = s.selected?.volume !== undefined ? [] : s.objects.flatMap((o) => o.instances.filter((i) => isSelected(s.selected, o.id, i.id)).map((i) => `${o.id}:${i.id}`));
  const inside = keys.every((k) => now.includes(k));
  setState({ selected: selectionOf(s, inside ? now.filter((k) => !keys.includes(k)) : [...now, ...keys.filter((k) => !now.includes(k))]) });
}

export function removeSelected() {
  if (state.selected?.all && !state.selected.picked) return deleteAll();
  if (state.selected?.volume !== undefined) return deleteVolume(state.selected.object, state.selected.volume);
  undoable('Delete selected', () => setState((s) => {
    if (!s.selected) return {};
    const objects = s.objects
      .map((o) => (o.instances.some((i) => isSelected(s.selected, o.id, i.id)) ? { ...o, instances: o.instances.filter((i) => !isSelected(s.selected, o.id, i.id)) } : o))
      .filter((o) => o.instances.length > 0);
    return { objects, selected: null, job: markStale(s.job) };
  }));
}

export function deleteAll() {
  const n = state.objects.length;
  undoable('Delete all', () => setState((s) => !s.project && s.projectPlates.length ? { objects: [], selected: null, job: markStale(s.job) }
    : { objects: [], selected: null, project: null, projectPlates: [], plateIndex: 1, wipeTowerPos: {}, projectName: null, projectMaterials: null, projectPresets: null, projectSlotValues: null, projectSlots: null, job: markStale(s.job) }));
  if (n) (n === 1 ? toast('deleted_object', undefined, { action: { label: 'Undo', run: undo } }) : toast('deleted_objects', { n }, { action: { label: 'Undo', run: undo } }));
}

export function setObjectOption(id: number, key: string, value: ConfigValue) {
  undoable(settingName(key), () => setState((s) => ({
    objects: s.objects.map((o) => (o.id === id ? { ...o, settings: { ...o.settings, [key]: value } } : o)),
    job: markStale(s.job),
  })), `object:${id}:${key}`);
}

export function removeObjectOptions(id: number, keys: string[]) {
  undoable('Delete Settings', () => setState((s) => ({
    objects: s.objects.map((o) => {
      if (o.id !== id) return o;
      const settings = { ...o.settings };
      for (const k of keys) delete settings[k];
      return { ...o, settings };
    }),
    job: markStale(s.job),
  })));
}

export function setEvents(events: LayerEvent[], name = 'Edit layer events') {
  undoable(name, () => setState((s) => ({ events: [...events].sort((a, b) => a.z - b.z), job: markStale(s.job) })));
}

export { markStale };

export function togglePrintable(objectId: number, instanceId?: number) {
  const o = state.objects.find((o) => o.id === objectId);
  if (!o) return;
  const printable = (o.instances.find((i) => i.id === instanceId) ?? o.instances[0]).printable === false;
  updateObject(objectId, { instances: o.instances.map((i) => instanceId === undefined || i.id === instanceId ? { ...i, printable } : i) },
    printable ? 'Set Printable' : 'Set Unprintable');
}

const mapVolumes = (s: State, objectId: number, f: (v: Volume[]) => Volume[]) =>
  s.objects.map((o) => (o.id === objectId ? { ...o, volumes: f(o.volumes ?? []) } : o));

export function updateVolume(objectId: number, volumeId: number, patch: Partial<Volume>, name = 'matrix' in patch ? 'Move' : editName(patch)) {
  undoable(name, () => setState((s) => ({
    objects: mapVolumes(s, objectId, (vs) => vs.map((v) => (v.id === volumeId ? { ...v, ...patch } : v))),
    job: markStale(s.job),
  })));
}

export function addVolume(objectId: number, volume: Volume, name: string) {
  undoable(name, () => setState((s) => {
    const o = s.objects.find((ob) => ob.id === objectId);
    if (!o) return {};
    const volumes = sortVolumes([...(o.volumes ?? []), volume]);
    return { objects: mapVolumes(s, objectId, () => volumes), selected: { object: objectId, instance: o.instances[0].id, volume: volume.id }, job: markStale(s.job) };
  }));
}

const sortVolumes = (vs: Volume[]) => vs.sort((a, b) => VOLUME_ORDER.indexOf(a.type) - VOLUME_ORDER.indexOf(b.type));

export function changeVolumeType(objectId: number, volumeId: number, type: VolumeType) {
  undoable('Change part type', () => setState((s) => ({
    objects: mapVolumes(s, objectId, (vs) => sortVolumes(vs.map((v) => (v.id === volumeId ? { ...v, type } : v)))),
    job: markStale(s.job),
  })));
}
export const VOLUME_ORDER: VolumeType[] = ['part', 'negative', 'modifier', 'support_blocker', 'support_enforcer', 'seam'];

export function deleteVolume(objectId: number, volumeId: number) {
  undoable('Delete part', () => setState((s) => ({
    objects: mapVolumes(s, objectId, (vs) => vs.filter((v) => v.id !== volumeId)),
    selected: s.selected?.volume === volumeId ? { object: objectId, instance: s.selected.instance, allInstances: true } : s.selected,
    job: markStale(s.job),
  })));
}

export function setVolumeOption(objectId: number, volumeId: number, key: string, value: ConfigValue) {
  undoable(settingName(key), () => setState((s) => ({
    objects: mapVolumes(s, objectId, (vs) => vs.map((v) => (v.id === volumeId ? { ...v, settings: { ...v.settings, [key]: value } } : v))),
    job: markStale(s.job),
  })), `volume:${volumeId}:${key}`);
}

export function removeVolumeOptions(objectId: number, volumeId: number, keys: string[]) {
  undoable('Delete Settings', () => setState((s) => ({
    objects: mapVolumes(s, objectId, (vs) => vs.map((v) => {
      if (v.id !== volumeId) return v;
      const settings = { ...v.settings };
      for (const k of keys) delete settings[k];
      return { ...v, settings };
    })),
    job: markStale(s.job),
  })));
}

observePreference('orca.settings_detail', () => { const mode = preferenceText('orca.settings_detail'); setState({uiMode: mode === 'advanced' || mode === 'develop' ? mode : 'simple'}); });
observePreference('orca.developer_mode', () => setState({developerMode: preferenceText('orca.developer_mode') === '1'}));
observePreference('orca.enable_high_low_temp_mixed_printing', () => { const next = preferenceText('orca.enable_high_low_temp_mixed_printing') === '1'; if (next !== getState().mixedTemperatures) setState(s=>({mixedTemperatures:next,job:markStale(s.job)})); });
