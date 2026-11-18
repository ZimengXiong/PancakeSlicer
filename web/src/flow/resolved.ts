import type { Config, ConfigValue, PresetType } from '../api/types';
import { createMachine, type Machine } from './fsm';
import { normalizeConfig } from '../config-values';
import { getState, setState, subscribeStore, type State } from '../state/store';
import { projectValues } from '../slice-input';
import { distinctAsks } from '../profiles/view';
import { slotFilaments, slotIds, slotMixes } from '../profiles/slots';
import { plateSlots } from '../printers/slot-check';
import { libraryWrites } from '../profiles/library-writes';


export type ResolvedLayer = 'default' | 'base' | 'tuning' | 'record' | 'auto' | 'printer' | 'calibration' | 'project' | 'object';
export type LayerValues = Partial<Record<ResolvedLayer, ConfigValue>>;
export interface ResolvedProfileValues { values: Config; layers: Record<string, LayerValues>; heights?: ResolvedHeights; nozzles?: ResolvedNozzleSpeeds[] }
export interface ResolvedNozzleSpeeds { extruder: number; name: string; diameter: number; variant: string; slots: number[]; flow?: number; speeds: Record<string, import('../profiles/speed-rules').SpeedDetail> }
export interface ResolvedHeights { chosen: number; available: number[]; limits: { extruder: number; diameter: number; max: number }[] }
export type ResolveStatus = { ok: true } | { ok: false; code: 'no_default_print' | 'unresolved' | 'unknown_record' | 'invalid_record'; message: string };

export interface ProfileAsk { type: PresetType; id: string; printer?: string; extruder?: number }
export interface DraftRecord { values: Config; scales?: import('../profiles/auto-rules').ScaleRow[]; speedModel?: import('../profiles/speed-rules').SpeedModel; preferredHeight?: number }
export interface ResolveRequest {
  printer: string; print: string; filaments: string[]; layerHeight: number | null;
  slots: string[];
  usedSlots: number[];
  project: { machine?: Config; process?: Config; filaments?: Config[]; slots?: Config };
  drafts?: { records?: Record<string, DraftRecord>; calibrations?: { filament: string; printer: string; extruder: number; values: Config }[] };
  profiles?: ProfileAsk[];
  objects?: { id: string; settings: Config }[];
}
export interface ResolveResponse {
  printer: ResolvedProfileValues & { status: ResolveStatus };
  print: ResolvedProfileValues & { status: ResolveStatus };
  filaments: (ResolvedProfileValues & { status: ResolveStatus })[];
  profiles?: (ResolvedProfileValues & ProfileAsk & { status: ResolveStatus; setup?: PlateSetup })[];
  objects?: (ResolvedProfileValues & { id: string; status: ResolveStatus })[];
  catalog: string; revision: number;
  setup?: PlateSetup;
}
export interface BuildPlate { value: string; label: string }
export interface FlushNozzle { matrix: number[]; auto: number[]; multiplier: number; minVolume: number }
export interface PlateSetup {
  plates: BuildPlate[]; default?: string; plate?: string;
  flushing?: { nozzles: FlushNozzle[]; own: boolean; stale?: boolean };
}

export type ResolveState = 'idle' | 'resolving' | 'ready' | 'failed';
interface ResolveContext { request: number; asked: ResolveRequest | null; pending: ResolveRequest | null; answer: ResolveResponse | null; answered: ResolveRequest | null; error: string | null; at: number }
type ResolveEvent = { type: 'INPUT'; request: ResolveRequest } | { type: 'ANSWER'; request: number; answer: ResolveResponse }
  | { type: 'FAIL'; request: number; error: string } | { type: 'RETRY' };
export type ResolvedSnapshot = Machine<ResolveState, ResolveEvent, ResolveContext>;

const same = (a: ResolveRequest | null, b: ResolveRequest | null) => JSON.stringify(a) === JSON.stringify(b);
const mine = (c: ResolveContext, e: { request: number }) => e.request === c.request;
const late = {} as const;

async function post(m: ResolvedSnapshot, request: number, body: ResolveRequest) {
  const started = performance.now();
  try {
    const r = await fetch('/api/resolve', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ ...body, revision: undefined }) });
    if (!r.ok) throw new Error((await r.json().catch(() => null))?.error ?? ([502, 503, 504].includes(r.status) ? 'Oops! Couldn\'t read the settings — the service is unreachable.' : `Oops! Couldn't read the settings (${r.status}).`));
    const answer = await r.json() as ResolveResponse;
    resolveTimings.push(performance.now() - started);
    m.send({ type: 'ANSWER', request, answer });
  } catch (e) { m.send({ type: 'FAIL', request, error: e instanceof Error ? e.message : String(e) }); }
}
export const resolveTimings: number[] = [];

const ask = { target: 'resolving', action: (c: ResolveContext, e: { request: ResolveRequest }) => ({ request: c.request + 1, asked: e.request, pending: null, error: null }) } as const;
export const createResolvedSnapshot = () => createMachine<ResolveState, ResolveEvent, ResolveContext>({
  id: 'resolved-snapshot', initial: 'idle', strict: true,
  context: { request: 0, asked: null, pending: null, answer: null, answered: null, error: null, at: 0 },
  states: {
    idle: { on: { INPUT: ask, ANSWER: late, FAIL: late, RETRY: late } },
    resolving: {
      entry: (c, _, m) => { void post(m, c.request, c.asked!); },
      on: {
        INPUT: [{ guard: (c, e) => same(c.asked, e.request), action: () => ({ pending: null }) }, { action: (_, e) => ({ pending: e.request }) }],
        ANSWER: [
          { target: 'resolving', guard: (c, e) => mine(c, e) && !!c.pending, action: (c, e) => ({ answer: e.answer, answered: c.asked, at: Date.now(), request: c.request + 1, asked: c.pending, pending: null }) },
          { target: 'ready', guard: mine, action: (c, e) => ({ answer: e.answer, answered: c.asked, at: Date.now(), error: null }) }, late],
        FAIL: [
          { target: 'resolving', guard: (c, e) => mine(c, e) && !!c.pending, action: (c) => ({ request: c.request + 1, asked: c.pending, pending: null }) },
          { target: 'failed', guard: mine, action: (_, e) => ({ error: e.error }) }, late],
        RETRY: late,
      },
    },
    ready: { on: { INPUT: [{ guard: (c, e) => same(c.asked, e.request) }, ask], ANSWER: late, FAIL: late, RETRY: late } },
    failed: { on: { INPUT: ask, RETRY: { target: 'resolving', action: (c) => ({ request: c.request + 1 }) }, ANSWER: late, FAIL: late } },
  },
});

export interface ResolvedView { state: ResolveState; answer: ResolveResponse | null; answeredFor: ResolveRequest | null; error: string | null }
export const NO_ANSWER: ResolvedView = { state: 'idle', answer: null, answeredFor: null, error: null };

export const resolvedSnapshot = createResolvedSnapshot();

export function resolveRequestOf(s: State): (ResolveRequest & { revision: number }) | null {
  if (!s.api || s.profileLibraryLoad.state !== 'ready') return null;
  if ((!s.printer || !s.process) && !s.resolveAsks.length) return null;
  return { printer: s.printer, print: s.process, filaments: [...slotFilaments(s)], slots: slotIds(s).map((slot) => slot ?? ''), usedSlots: plateSlots(s), layerHeight: s.processVariant?.layerHeight ?? null,
    project: projectValues(s), ...(s.resolveAsks.length ? { profiles: distinctAsks(s.resolveAsks) } : {}), ...objectSettings(s), revision: s.records.revision };
}

export function selectionAnswer(): Promise<ResolveResponse> {
  return new Promise((resolve, reject) => {
    const settled = (): boolean => {
      const s = getState(), r = s.resolved;
      if (!s.api) return reject(new Error('Oops, we can\'t reach the slicing service right now, so we can\'t tell the printer\'s bed size.')), true;
      if (s.profileLibraryLoad.state === 'failed') return reject(new Error(`Uh-oh, your profile library didn't load (${s.profileLibraryLoad.error}), so we can't tell the printer's bed size.`)), true;
      if (r.state === 'failed') return reject(new Error(r.error ?? 'Oops! Couldn\'t read the printer\'s settings.')), true;
      const request = resolveRequestOf(s);
      if (r.state !== 'ready' || !r.answer || !request || !same(r.answeredFor, request)) return false;
      resolve(r.answer);
      return true;
    };
    if (settled()) return;
    const stop = subscribeStore(() => { if (settled()) stop(); });
  });
}

function objectSettings(s: State): { objects?: { id: string; settings: Config }[] } {
  const objects = s.objects.filter((o) => Object.keys(o.settings).length).map((o) => ({ id: String(o.id), settings: o.settings }));
  return objects.length ? { objects } : {};
}
const values = (p: ResolvedProfileValues) => ({ ...p, values: normalizeConfig(p.values, true) });
function normalized(a: ResolveResponse): ResolveResponse {
  return { ...a, printer: { ...values(a.printer), status: a.printer.status }, print: { ...values(a.print), status: a.print.status },
    filaments: a.filaments.map((f) => ({ ...values(f), status: f.status })), profiles: a.profiles?.map((p) => ({ ...p, ...values(p) })), objects: a.objects?.map((o) => ({ ...o, ...values(o) })) };
}

let watching = false;
let answerSource: ResolveResponse | null = null;
export function watchResolved(): void {
  if (watching) return;
  watching = true;
  resolvedSnapshot.subscribe(({ value, context }) => { const prior = getState().resolved;
    setState({ resolved: { state: value, answer: context.answer === answerSource ? prior.answer : (answerSource = context.answer) && normalized(context.answer!), answeredFor: context.answered, error: context.error } }); });
  const inputs = (s: State) => [s.api, s.profileLibraryLoad, s.printer, s.process, slotFilaments(s), s.processVariant, s.projectPresets, s.dirty, s.filamentEdits,
    s.projectMaterials, s.projectPlates, s.plateIndex, s.wipeTowerPos, slotMixes(s), s.records, s.project, s.resolveAsks, s.objects, s.events];
  let last: unknown[] = [];
  const check = () => {
    if (libraryWrites.get().value === 'writing') return;
    const s = getState(), now = inputs(s);
    if (now.length === last.length && now.every((v, i) => v === last[i])) return;
    last = now;
    const request = resolveRequestOf(s);
    if (request) resolvedSnapshot.send({ type: 'INPUT', request });
  };
  subscribeStore(() => queueMicrotask(check)); libraryWrites.subscribe(check); check();
}
