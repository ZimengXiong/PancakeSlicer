import type { Config } from './api/types';
import type { ObjectResult } from './preview/explain-record';
import { applyStyle } from './print-style';
import { beginStep, undoable } from './state/history';
import { onShellsChecked, topSlopes } from './plater/geometry';
import { effectiveConfig, getState, setOption, setState, subscribeStore, type PlateObject, type State } from './state/store';
import { styleOf } from './style-names';
import { bareName } from './engine-warnings';

function refresh() {
  const s = getState();
  const next: State['recommendedStyle'] = {};
  for (const o of [...s.objects, ...s.projectPlates.flatMap((p) => p.objects)])
    if (o.geometry.userData.printInPlaceGap != null) next[o.id] = 'standard';
  const prev = s.recommendedStyle;
  const keys = Object.keys(next);
  if (keys.length !== Object.keys(prev).length || keys.some((k) => prev[+k] !== next[+k])) setState({ recommendedStyle: next });
}
onShellsChecked(refresh);
subscribeStore(refresh);

let explain: typeof import('./preview/explain-record') | null = null;
export const explainRecord = () => explain;
subscribeStore(() => {
  if (explain || getState().job.state !== 'done') return;
  void import('./preview/explain-record').then((m) => { if (!explain) { explain = m; kept = null; setState({}); } });
});

export function pendingRecommendations(s: State): PlateObject[] {
  const global = effectiveConfig(s, 'process');
  return s.objects.filter((o) => s.recommendedStyle[o.id] && styleOf({ ...global, ...o.settings }) !== s.recommendedStyle[o.id]);
}

export function applyRecommended(objects: PlateObject[]) {
  if (!objects.length) return;
  const end = beginStep('Change print style to "Standard"');
  try {
    setState({ selected: { object: objects[0].id, instance: objects[0].instances[0].id } });
    for (const o of objects) applyStyle(getState().recommendedStyle[o.id], o.id);
  } finally {
    end();
  }
}

export interface PlateResult extends ObjectResult { objectId?: number; label: string; flat?: string; partial?: string }

const NONE: PlateResult[] = [];
let kept: { record: unknown; objects: unknown; rec: unknown; out: PlateResult[] } | null = null;
function flatReason(r: ObjectResult, o: PlateObject | undefined, process: Config): { why: string; hint?: string } | null {
  if (!r.limit) return null;
  const slopes = o && topSlopes(o), whole = (v: number) => String(Math.round(v));
  const slope = slopes && slopes.max >= 0.5 ? `Top slope ${slopes.max - slopes.min > 1 ? 'up to ' : ''}${whole(slopes.max)}°` : 'Top';
  const { kind, capDeg, reachableDeg } = r.limit;
  if (kind === 'access') {
    const moves = r.flatWhy?.match(/(?:met|would bump) the print head in (\d+) moves/)?.[1];
    return { why: reachableDeg != null ? `${slope} is steeper than the ${r.limit.accessBy === 'nozzle' ? 'nozzle can follow' : 'print head reaches'} (${whole(reachableDeg)}°)`
      : moves ? `The print head would bump into the part in ${Number(moves).toLocaleString('en-US')} places` : 'The print head doesn\'t have enough clearance to reach this top' };
  }
  if (kind === 'angle') return { why: `${slope} is above your ${capDeg != null ? `${whole(capDeg)}° ` : ''}curved-top limit`, hint: 'try increasing Max curved top angle' };
  if (kind === 'area') {
    const area = Number({ ...process, ...o?.settings }.nonplanar_curved_min_area);
    return { why: `Smaller than your ${Number.isFinite(area) ? `${area} mm² ` : ''}Minimum curved top area setting` };
  }
  if (kind === 'base') return { why: 'Too close to the print bed for curved layers' };
  if (kind === 'budget') return { why: 'Curving this surface would add too much print time without improving quality' };
  if (kind === 'hits') {
    const moves = r.flatWhy?.match(/(\d+) moves/)?.[1];
    return { why: moves ? `The print head would bump into the part in ${Number(moves).toLocaleString('en-US')} places` : 'The print head would bump into the part' };
  }
  return null;
}

function topFact(r: ObjectResult, o: PlateObject, process: Config): string {
  if (r.curved || r.ledge) return r.fact;
  const f = flatReason(r, o, process);
  return f ? `${f.why}, prints as Standard${f.hint ? `; ${f.hint}` : ''}` : r.fact;
}

function flatLine(r: ObjectResult, o: PlateObject | undefined, process: Config): string {
  const f = flatReason(r, o, process);
  return f ? `${f.why}${f.hint ? `; ${f.hint}` : ''}` : r.flatWhy || 'No top surfaces fit within your current curved-layer settings';
}

function partialLine(r: ObjectResult): string | undefined {
  if (!r.curved || r.printedFlat || !r.tops || r.tops.kept >= r.tops.candidates || r.tops.kept < 1) return undefined;
  const whole = (v: number) => String(Math.round(v));
  const limit = r.limit;
  const byLimit = !limit ? undefined
    : limit.kind === 'access' ? (limit.reachableDeg != null ? `steeper than the ${limit.accessBy === 'nozzle' ? 'nozzle can follow' : 'print head reaches'} (${whole(limit.reachableDeg)}°)` : 'the print head cannot reach them')
    : limit.kind === 'angle' ? `above your ${limit.capDeg != null ? `${whole(limit.capDeg)}° ` : ''}curved-top limit`
    : limit.kind === 'area' ? 'smaller than your Minimum curved top area'
    : limit.kind === 'base' ? 'too close to the bed'
    : 'a curved top would cost too much time or quality';
  const budget: Record<string, string> = { firmware_time_budget: 'a curved top would cost too much time', top_rms_worse_than_planar: 'flat layers print them smoother' };
  const reasons = [...new Set([byLimit, ...r.tops.dropped.map((d) => budget[d])].filter((w): w is string => !!w))];
  if (!reasons.length) return undefined;
  const n = r.tops.candidates - r.tops.kept;
  return `${n} ${n === 1 ? 'top' : 'tops'} flat: ${reasons.join('; ')}`;
}

export interface CurveFailure { objectId?: number; label: string; method: string; why: string; tryDuct: boolean;
  mode: string; kind: string }
type RecordFailure = { name?: string; method?: string; mode?: string; planar_limit?: string; reason?: string; try_ignore_duct?: boolean };
const NO_FAILURES: CurveFailure[] = [];
let keptFailures: { results: PlateResult[]; record: unknown; out: CurveFailure[] } | null = null;
export function curveFailures(s: State): CurveFailure[] {
  const record = s.job.state === 'done' && !s.job.stale ? s.job.status?.record as { could_not_curve?: RecordFailure[] } | undefined : undefined;
  const listed = record?.could_not_curve;
  if (!listed?.length) return NO_FAILURES;
  const results = plateResults(s);
  if (keptFailures && keptFailures.record === record && keptFailures.results === results) return keptFailures.out;
  const bare = bareName;
  const out = listed.map((f): CurveFailure => {
    const name = String(f.name ?? '');
    const r = results.find((p) => bare(p.name) === bare(name)) ?? (results.length === 1 && listed.length === 1 ? results[0] : undefined);
    const o = r?.objectId != null ? s.objects.find((p) => p.id === r.objectId) : s.objects.length === 1 ? s.objects[0] : s.objects.find((p) => bare(p.name) === bare(name));
    const engine = String(f.reason ?? '').replace(/;\s*printed with flat layers\.?$/, '');
    const why = r?.flat ?? (r?.limit ? flatReason(r, o, effectiveConfig(s, 'process'))?.why : undefined) ?? (engine || 'no top surface could be curved with the current limits');
    return { objectId: o?.id, label: bare(o?.name ?? r?.label ?? name), method: String(f.method ?? r?.style ?? 'this method'), why: why[0].toLowerCase() + why.slice(1), tryDuct: f.try_ignore_duct === true,
      mode: String(f.mode ?? ''), kind: String(f.planar_limit ?? '') };
  });
  keptFailures = { results, record, out };
  return out;
}
export const hasCurveFailure = (s: State) => curveFailures(s).length > 0;

export function slicedFlatReason(s: State, objectId: number): { why: string; head: boolean; tryDuct: boolean; limit?: ObjectResult['limit'] } | null {
  const failure = curveFailures(s).find((f) => f.objectId === objectId);
  const r = plateResults(s).find((p) => p.objectId === objectId);
  const o = s.objects.find((p) => p.id === objectId);
  const why = failure?.why ?? (r ? flatReason(r, o, effectiveConfig(s, 'process'))?.why ?? r.flatWhy : undefined);
  if (!why) return null;
  return { why: why[0].toUpperCase() + why.slice(1), head: (r?.limit?.kind === 'access' && r.limit.accessBy !== 'nozzle') || !!failure?.tryDuct, tryDuct: !!failure?.tryDuct, limit: r?.limit };
}

const sliceAgain = () => void import('./actions').then((m) => m.slice());

export function sliceFlatInstead(failures: CurveFailure[]) {
  const ids = failures.flatMap((f) => (f.objectId != null ? [f.objectId] : []));
  const end = beginStep('Slice flat instead');
  try {
    if (ids.length) for (const id of ids) applyStyle('standard', id);
    else applyStyle('standard');
  } finally {
    end();
  }
  sliceAgain();
}

export function trySmoothest(f: CurveFailure) {
  if (f.objectId != null) applyStyle('smooth', f.objectId);
  else applyStyle('smooth');
  sliceAgain();
}

export function tryIgnoreDuct() {
  undoable('Ignore cooling duct', () => setOption('process', 'nonplanar_ignore_duct', '1'));
  sliceAgain();
}

export function plateResults(s: State): PlateResult[] {
  const record = s.job.state === 'done' && !s.job.stale ? s.job.status?.record : undefined;
  if (kept && kept.record === record && kept.objects === s.objects && kept.rec === s.recommendedStyle) return kept.out;
  if (record && !explain) return NONE;
  const results = explain?.objectResults(record) ?? [];
  const bare = (n: string) => n.replace(/\.(stl|obj|3mf|step|stp)$/i, '');
  const process = effectiveConfig(s, 'process');
  const out = results.flatMap((r) => {
    const o = s.objects.length === 1 && results.length === 1 ? s.objects[0] : s.objects.find((p) => bare(p.name) === bare(r.name));
    const partial = partialLine(r);
    return [{ ...r, fact: o ? topFact(r, o, process) : r.fact, objectId: o?.id, label: bareName(o?.name ?? r.name), ...(r.printedFlat ? { flat: flatLine(r, o, process) } : {}), ...(partial ? { partial } : {}) }];
  });
  kept = { record, objects: s.objects, rec: s.recommendedStyle, out };
  return out;
}
