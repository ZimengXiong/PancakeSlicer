import { useSyncExternalStore } from 'react';
import { joinParts } from './separator';
import * as THREE from 'three';
import { mutation } from './api/access';
import { jobRequest } from './actions';
import { withBlobs } from './api/blobs';
import { bakeStl, instanceMatrix } from './plater/geometry';
import { loadMoves, type PreviewData } from './preview/data';
import { MoveType } from './preview/format';
import { slicedFlatReason } from './recommend';
import { plateSignature } from './slice-input';
import { effectiveConfig, getState, subscribeStore, toast, type PlateObject, type State } from './state/store';

export type SurfaceStatus = 'curves' | 'duct' | 'steep' | 'small' | 'flat';
export type Verdict = 'curves' | 'duct' | 'no' | 'flat';
export interface Surface { estimate: SurfaceStatus; reason: string; slope: number; span: number; area: number;
  verdict: Verdict;
  line: string; why?: string;
  share?: number;
  base: Pick<Surface, 'verdict' | 'line' | 'why' | 'share'> }
export interface InstanceSurfaces { objectId: number; instanceId: number; source: THREE.BufferGeometry; surfaces: Surface[];
  regionOf: Int32Array; geometry: THREE.BufferGeometry;
  overlayRegion: Int32Array;
  curvedTri: Uint8Array | null }

type RGB = [number, number, number];
const COLOURS: Record<Verdict, RGB | null> = {
  curves: [22 / 255, 179 / 255, 160 / 255],
  duct: [245 / 255, 165 / 255, 36 / 255],
  no: [190 / 255, 96 / 255, 86 / 255],
  flat: null,
};
export const SURFACE_LEGEND: [string, string][] = [['#16B3A0', 'Curves'], ['#F5A524', 'Needs Ignore cooling duct'], ['#BE6056', "Can't curve this spot"]];

function estimated(status: SurfaceStatus, reason: string, share?: number): Pick<Surface, 'verdict' | 'line' | 'why' | 'share'> {
  const likely = source === 'plan' ? '' : 'Likely ';
  if (status === 'flat') return { verdict: 'flat', line: 'Already flat (prints cleanly)', share: undefined };
  if (status === 'curves') return { verdict: 'curves', line: `${likely}curves`.replace(/^./, (c) => c.toUpperCase()), share };
  if (status === 'duct') return { verdict: 'duct', line: `${likely}needs Ignore cooling duct`.replace(/^./, (c) => c.toUpperCase()), why: source === 'plan' ? reason : undefined, share };
  return { verdict: 'no', line: "Can't curve this spot", why: reason, share };
}
let source: 'estimate' | 'plan' = 'estimate';

let shown = false, loading = false, ductIgnorable = false;
let slicedBy: string | null = null;
let current: InstanceSurfaces[] = [];
let hover: { instanceId: number; region: number } | null = null;
let selected: { instanceId: number; region: number } | null = null;
const listeners = new Set<() => void>();
const emit = () => listeners.forEach((l) => l());
export const subscribeTopSurfaces = (l: () => void) => (listeners.add(l), () => void listeners.delete(l));
export const topSurfacesShown = () => shown;
export const topSurfaces = () => current;
export const useTopSurfaces = () => useSyncExternalStore(subscribeTopSurfaces, () => (shown ? (loading ? 'loading' : 'on') : 'off'));
export const useSurfaceSource = () => useSyncExternalStore(subscribeTopSurfaces, () => (slicedBy && current.length ? 'slice' : source));

export function toggleTopSurfaces(on = !shown) {
  if (on === shown) return;
  shown = on;
  if (!on) hover = selected = null;
  emit();
  sync();
}

window.addEventListener('keydown', (e) => {
  if (!shown || e.key !== 'Escape' || document.querySelector('[role="dialog"], [role="alertdialog"], [role="menu"]')) return;
  const t = e.target as HTMLElement | null;
  if (t && (t.isContentEditable || /^(INPUT|TEXTAREA|SELECT)$/.test(t.tagName))) return;
  toggleTopSurfaces(false);
});

function settingsOf(s: State, o: PlateObject) {
  const c = { ...effectiveConfig(s, 'process'), ...o.settings };
  const num = (k: string) => Number(Array.isArray(c[k]) ? (c[k] as unknown[])[0] : c[k]);
  return { mode: String(c.nonplanar_mode ?? 'off'), max_angle: num('nonplanar_curved_max_angle') || 30,
    min_area: num('nonplanar_min_patch_area') || 0, ignore_duct: ['1', 'true'].includes(String(c.nonplanar_ignore_duct ?? '0')) };
}

const wanted = (s: State) => s.objects.filter((o) => !o.project && !o.native && o.geometry);

function triangles(source: THREE.BufferGeometry) {
  return (source.index ? source.index.count : source.attributes.position.count) / 3;
}

function decodeRuns(b64: string): number[] {
  const bytes = Uint8Array.from(atob(b64), (c) => c.charCodeAt(0));
  const runs = new Uint32Array(bytes.buffer, 0, bytes.byteLength >> 2), out: number[] = [];
  for (let k = 0; k + 1 < runs.length; k += 2) for (let n = 0; n < runs[k + 1]; n++) out.push(runs[k] + n);
  return out;
}

const tint = (rgb: [number, number, number], lift: number): [number, number, number] => rgb.map((v) => v + (1 - v) * lift) as [number, number, number];

type RegionIn = { facets: string; status: SurfaceStatus; reason: string; slope_max: number; span: number; area: number; share?: number };
function build(o: PlateObject, instanceId: number, regions: RegionIn[]): InstanceSurfaces {
  const source = o.geometry, pos = source.attributes.position, index = source.index, count = triangles(source);
  const regionOf = new Int32Array(count).fill(-1);
  const surfaces: Surface[] = [];
  const tris: number[] = [], owner: number[] = [];
  regions.forEach((r, k) => {
    const base = estimated(r.status, r.reason, r.share);
    surfaces.push({ estimate: r.status, reason: r.reason, slope: r.slope_max, span: r.span, area: r.area, ...base, base });
    for (const t of decodeRuns(r.facets)) {
      if (t >= count || regionOf[t] >= 0) continue;
      regionOf[t] = k;
      if (r.status !== 'flat') tris.push(t), owner.push(k);
    }
  });
  const p = new Float32Array(tris.length * 9), col = new Float32Array(tris.length * 9);
  tris.forEach((t, i) => {
    for (let c = 0; c < 3; c++) {
      const v = index ? index.getX(3 * t + c) : 3 * t + c;
      p[9 * i + 3 * c] = pos.getX(v); p[9 * i + 3 * c + 1] = pos.getY(v); p[9 * i + 3 * c + 2] = pos.getZ(v);
    }
  });
  const geometry = new THREE.BufferGeometry().setAttribute('position', new THREE.BufferAttribute(p, 3)).setAttribute('color', new THREE.BufferAttribute(col, 3));
  geometry.computeBoundingSphere();
  const out: InstanceSurfaces = { objectId: o.id, instanceId, source, surfaces, regionOf, geometry, overlayRegion: Int32Array.from(owner), curvedTri: null };
  paint(out);
  return out;
}

function paint(inst: InstanceSurfaces, only?: number) {
  const col = inst.geometry.attributes.color as THREE.BufferAttribute, a = col.array as Float32Array;
  inst.overlayRegion.forEach((k, i) => {
    if (only !== undefined && k !== only) return;
    const f = inst.surfaces[k];
    const base = inst.curvedTri ? (inst.curvedTri[i] ? COLOURS.curves! : COLOURS[f.verdict === 'curves' || f.verdict === 'flat' ? 'no' : f.verdict]!) : COLOURS[f.verdict] ?? COLOURS.no!;
    const lift = selected?.instanceId === inst.instanceId && selected.region === k ? 0.45 : hover?.instanceId === inst.instanceId && hover.region === k ? 0.25 : 0;
    const [r, g, b] = lift ? tint(base, lift) : base;
    for (let v = 0; v < 3; v++) a.set([r, g, b], 9 * i + 3 * v);
  });
  col.needsUpdate = true;
}

export function surfaceAt(instanceId: number, tri: number | undefined): { inst: InstanceSurfaces; region: number } | null {
  if (!shown || tri === undefined) return null;
  const inst = current.find((i) => i.instanceId === instanceId);
  const region = inst ? inst.regionOf[tri] ?? -1 : -1;
  return inst && region >= 0 ? { inst, region } : null;
}

const same = (a: typeof hover, b: typeof hover) => a?.instanceId === b?.instanceId && a?.region === b?.region;
function recolour(prev: typeof hover, next: typeof hover) {
  for (const h of [prev, next]) {
    const inst = h && current.find((i) => i.instanceId === h.instanceId);
    if (inst && h) paint(inst, h.region);
  }
}
export function hoverSurface(instanceId: number | null, region = -1) {
  const next = instanceId === null || region < 0 ? null : { instanceId, region };
  if (same(hover, next)) return false;
  const prev = hover;
  hover = next;
  recolour(prev, next);
  return true;
}
export function selectSurface(instanceId: number | null, region = -1) {
  const next = instanceId === null || region < 0 ? null : { instanceId, region };
  if (same(selected, next)) return;
  const prev = selected;
  selected = next;
  recolour(prev, next);
  emit();
}
export const selectedSurface = () => selected;

export function surfaceText(f: Surface): string[] {
  const area = f.area < 10 ? f.area.toFixed(1) : String(Math.round(f.area));
  const share = f.share !== undefined && f.share > 0 && f.share < 0.995 ? ` (${Math.round(f.share * 100)}% curved)` : '';
  return [f.line + share, ...(f.why ? [f.why] : []), joinParts([`Max slope ${Math.round(f.slope)}°`, `${f.span.toFixed(1)} mm tall`, `${area} mm²`])];
}
export const headIgnoresDuct = () => ductIgnorable;

const CELL = 0.5;
const cellKey = (x: number, y: number) => Math.round(x / CELL) * 100003 + Math.round(y / CELL);
let curvedMoves: { jobId: string; cells: Map<number, number[]> } | null = null;
let movesFor: string | null = null;
let cancelMoves: (() => void) | null = null;

function cellsOf(d: PreviewData): Map<number, number[]> {
  const cells = new Map<number, number[]>(), st = d.states!;
  const put = (x: number, y: number, z: number) => {
    const k = cellKey(x, y), c = cells.get(k);
    if (c) c.push(z); else cells.set(k, [z]);
  };
  const isCurved = (i: number) => (d.type[i] & 0x7f) === MoveType.Extrude && st.shell[d.state[i]] !== 0xff;
  const range = new Map<number, [number, number]>();
  for (let i = 1; i < d.loaded; ++i) {
    if (!isCurved(i)) continue;
    const key = st.patch[d.state[i]] * 256 + st.shell[d.state[i]], z = d.pos[i * 4 + 2], r = range.get(key);
    if (r) { r[0] = Math.min(r[0], z); r[1] = Math.max(r[1], z); } else range.set(key, [z, z]);
  }
  const kept = new Set<number>();
  for (const [key, [lo, hi]] of range) if (hi - lo >= 0.03 - 1e-6) kept.add(Math.floor(key / 256));
  let prev = -1;
  for (let i = 1; i < d.loaded; ++i) {
    const patch = st.patch[d.state[i]];
    const curved = isCurved(i) && (patch < 0 || kept.has(patch));
    if (!curved) { prev = -1; continue; }
    const x = d.pos[i * 4], y = d.pos[i * 4 + 1], z = d.pos[i * 4 + 2];
    put(x, y, z);
    if (prev >= 0) {
      const px = d.pos[prev * 4], py = d.pos[prev * 4 + 1], pz = d.pos[prev * 4 + 2];
      const n = Math.min(200, Math.floor(Math.hypot(x - px, y - py) / (CELL * 0.8)));
      for (let k = 1; k < n; ++k) { const t = k / n; put(px + (x - px) * t, py + (y - py) * t, pz + (z - pz) * t); }
    }
    prev = i;
  }
  return cells;
}

let signatureMemo: { objects: unknown; job: unknown; dirty: unknown; printer: unknown; process: unknown; value: string } | null = null;
function slicedJob(s: State): string | null {
  if (s.job.state !== 'done' || s.job.stale || !s.job.id || !s.job.plateSignature || !s.api || s.api.mock) return null;
  const m = signatureMemo;
  const value = m && m.objects === s.objects && m.job === s.job && m.dirty === s.dirty && m.printer === s.printer && m.process === s.process ? m.value : plateSignature(s);
  signatureMemo = { objects: s.objects, job: s.job, dirty: s.dirty, printer: s.printer, process: s.process, value };
  return value === s.job.plateSignature ? s.job.id : null;
}

function syncSlice(s: State) {
  const id = shown ? slicedJob(s) : null;
  if (id === movesFor) return;
  cancelMoves?.();
  cancelMoves = null;
  movesFor = id;
  if (!id) {
    if (curvedMoves) { curvedMoves = null; applyOutcome(); }
    return;
  }
  const h = loadMoves({ url: s.api!.movesUrl(id) }, {
    header() {}, chunk() {},
    done: (d) => { if (movesFor === id) { curvedMoves = { jobId: id, cells: cellsOf(d) }; cancelMoves = null; applyOutcome(); } },
    error: () => { if (movesFor === id) cancelMoves = null; },
  });
  cancelMoves = h.cancel;
}

function applyOutcome() {
  const s = getState();
  slicedBy = curvedMoves?.jobId ?? null;
  const a = new THREE.Vector3(), b = new THREE.Vector3(), c = new THREE.Vector3();
  for (const inst of current) {
    const o = s.objects.find((p) => p.id === inst.objectId), placed = o?.instances.find((i) => i.id === inst.instanceId);
    if (!curvedMoves || !o || !placed) {
      inst.curvedTri = null;
      inst.surfaces.forEach((f, k) => { inst.surfaces[k] = { ...f, ...f.base }; });
      paint(inst);
      continue;
    }
    const m = instanceMatrix(o, placed), p = inst.geometry.attributes.position.array as Float32Array;
    const curved = new Uint8Array(inst.overlayRegion.length), done = new Float64Array(inst.surfaces.length), total = new Float64Array(inst.surfaces.length);
    for (let i = 0; i < curved.length; ++i) {
      a.fromArray(p, 9 * i).applyMatrix4(m); b.fromArray(p, 9 * i + 3).applyMatrix4(m); c.fromArray(p, 9 * i + 6).applyMatrix4(m);
      const area = Math.abs((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y)) / 2;
      const x = (a.x + b.x + c.x) / 3, y = (a.y + b.y + c.y) / 3, z = (a.z + b.z + c.z) / 3;
      let hit = false;
      for (let dx = -1; dx <= 1 && !hit; ++dx) for (let dy = -1; dy <= 1 && !hit; ++dy) {
        const zs = curvedMoves.cells.get(cellKey(x + dx * CELL, y + dy * CELL));
        if (zs) for (const v of zs) if (v > z - 0.8 && v < z + 0.5) { hit = true; break; }
      }
      curved[i] = hit ? 1 : 0;
      total[inst.overlayRegion[i]] += area;
      if (hit) done[inst.overlayRegion[i]] += area;
    }
    inst.curvedTri = curved;
    const flat = slicedFlatReason(s, o.id);
    const ignoring = settingsOf(s, o).ignore_duct;
    inst.surfaces.forEach((f, k) => {
      if (f.estimate === 'flat') return;
      const share = total[k] > 0 ? done[k] / total[k] : 0;
      const deg = (v: number) => `${Math.round(v)}°`, limit = flat?.limit;
      const why = f.estimate === 'small' ? f.reason
        : limit?.kind === 'access' && limit.reachableDeg != null ? `Slope reaches ${deg(f.slope)}, which is steeper than the ${limit.accessBy === 'nozzle' ? 'nozzle can follow' : 'print head reaches'} limit (${deg(limit.reachableDeg)})`
        : limit?.kind === 'angle' && limit.capDeg != null ? `Slope reaches ${deg(f.slope)}, which is above your ${deg(limit.capDeg)} curved-top limit`
        : flat?.why ?? 'Kept flat during slicing';
      inst.surfaces[k] = share >= 0.5 ? { ...f, verdict: 'curves', line: 'Curves', why: undefined, share }
        : (flat?.tryDuct || (flat?.head && ductIgnorable && !ignoring)) && f.estimate !== 'small'
          ? { ...f, verdict: 'duct', line: 'Needs Ignore cooling duct', why, share }
          : { ...f, verdict: 'no', line: "Can't curve this spot", why, share };
    });
    paint(inst);
  }
  emit();
}

interface PlanRegion { object: number; facets: string; curved: boolean; curved_share: number; planar_limit: string | null; reachable_slope_deg: number | null;
  access_by?: 'nozzle' | 'head'; ignore_duct?: { curved: boolean }; area_xy: number; span: number; slope_max: number }
interface PlanTops { schema: string; ignore_duct: boolean; regions: PlanRegion[] }

function planReason(r: PlanRegion): string {
  const slope = `Slope up to ${Math.round(r.slope_max)}°`, deg = r.reachable_slope_deg != null ? `${Math.round(r.reachable_slope_deg)}°` : null;
  switch (r.planar_limit) {
    case 'access': return deg ? `${slope} is steeper than the ${r.access_by === 'nozzle' ? 'nozzle can follow' : 'print head reaches'} (${deg})` : 'The print head can\'t reach this area safely';
    case 'angle': return deg ? `${slope} is above the ${deg} curved-top limit` : `${slope} is above the curved-top limit`;
    case 'area': return 'Area is too small for curved top layers';
    case 'head_unknown': return 'No print head model set for this printer';
    case 'flat': return 'Already flat (prints cleanly)';
    default: return 'Planned as flat layers';
  }
}

let planJob: string | null = null;
async function planned(s: State, objects: PlateObject[], mine: number): Promise<InstanceSurfaces[]> {
  const req = { ...(await jobRequest(s)), events: [], output: 'gcode' as const, plan_tops: true };
  const id = await s.api!.submit(req);
  if (planJob) void s.api!.cancel(planJob).catch(() => undefined);
  planJob = id;
  const status = await new Promise<{ record?: unknown }>((resolve, reject) => {
    const stop = s.api!.events(id, { progress() {}, done(st) { stop(); resolve(st); }, error(m) { stop(); reject(new Error(m)); } });
  });
  if (planJob === id) planJob = null;
  if (mine !== generation) return [];
  const plan = status.record as PlanTops | undefined;
  if (plan?.schema !== 'orca.nonplanar.plan_tops.v1') throw new Error('the slicer didn\'t return a curved-tops preview');
  return objects.flatMap((o) => {
    const oi = s.objects.indexOf(o);
    const regions: RegionIn[] = plan.regions.filter((r) => r.object === oi).map((r) => {
      const status: SurfaceStatus = r.curved ? 'curves' : r.planar_limit === 'flat' ? 'flat' : !plan.ignore_duct && r.ignore_duct?.curved ? 'duct'
        : r.planar_limit === 'area' ? 'small' : 'steep';
      return { facets: r.facets, status, reason: planReason(r), slope_max: r.slope_max, span: r.span, area: r.area_xy, share: r.curved_share };
    });
    return o.instances.map((inst) => build(o, inst.id, regions));
  });
}

let timer: ReturnType<typeof setTimeout> | undefined;
let lastKey = '';
let generation = 0;

async function request() {
  const mine = ++generation;
  const s = getState();
  const objects = wanted(s);
  const head = String(effectiveConfig(s, 'machine').nonplanar_head ?? '');
  if (!objects.length || !s.api || s.api.mock) {
    current = [];
    loading = false;
    emit();
    return;
  }
  loading = true;
  emit();
  try {
    if (!s.project) {
      source = 'plan';
      const built = await planned(s, objects, mine);
      if (mine !== generation) return built.forEach((b) => b.geometry.dispose());
      current.forEach((c) => c.geometry.dispose());
      current = built;
      hover = selected = null;
      if (curvedMoves) applyOutcome();
      return;
    }
    source = 'estimate';
    const groups = new Map<string, PlateObject[]>();
    for (const o of objects) {
      const k = JSON.stringify(settingsOf(s, o));
      groups.set(k, [...(groups.get(k) ?? []), o]);
    }
    const built: InstanceSurfaces[] = [];
    await Promise.all([...groups].map(async ([k, group]) => {
      const blobs = await Promise.all(group.map((o) => bakeStl(o)));
      const answer = await withBlobs(blobs, async (sums) => {
        const meshes = group.flatMap((o, i) => o.instances.map((inst) => ({ sha256: sums[i], rot_z: inst.rotZ })));
        const r = await mutation('/api/top-surfaces', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ ...JSON.parse(k), head, meshes }) });
        return r.json() as Promise<{ duct_ignorable: boolean; objects: { error?: string; regions?: Parameters<typeof build>[2] }[] }>;
      });
      ductIgnorable = answer.duct_ignorable;
      let n = 0;
      for (const o of group) for (const inst of o.instances) {
        const a = answer.objects[n++];
        if (a?.error) throw new Error(a.error);
        if (a?.regions) built.push(build(o, inst.id, a.regions));
      }
    }));
    if (mine !== generation) return built.forEach((b) => b.geometry.dispose());
    current.forEach((c) => c.geometry.dispose());
    current = built;
    hover = selected = null;
    if (curvedMoves) applyOutcome();
  } catch (e) {
    if (mine !== generation) return;
    toast('top_surfaces', { detail: e instanceof Error ? e.message : e });
    lastKey = '';
  } finally {
    if (mine === generation) {
      loading = false;
      emit();
    }
  }
}

function sync() {
  const s = getState();
  syncSlice(s);
  if (!shown || s.tab !== 'prepare') {
    clearTimeout(timer);
    if (!shown && current.length) {
      generation++;
      current.forEach((c) => c.geometry.dispose());
      current = [];
      lastKey = '';
      loading = false;
      emit();
    }
    return;
  }
  const objects = wanted(s);
  const key = JSON.stringify([String(effectiveConfig(s, 'machine').nonplanar_head ?? ''), objects.map((o) => [o.id, o.geometry.uuid,
    (o.geometry.attributes.position as THREE.BufferAttribute).version, o.rotX, o.rotY, o.scale, o.instances.map((i) => [i.id, i.rotZ]), settingsOf(s, o)])]);
  if (key === lastKey) return;
  lastKey = key;
  clearTimeout(timer);
  timer = setTimeout(() => void request(), current.length ? 250 : 0);
}
subscribeStore(sync);
(window as unknown as { __topSurfaces: unknown }).__topSurfaces = () => ({ shown, loading, slicedBy, current: current.map((c) => ({ objectId: c.objectId, instanceId: c.instanceId, surfaces: c.surfaces })) });
