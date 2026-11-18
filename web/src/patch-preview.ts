import { observePreference, preferenceText, savePreferences } from './saved-preferences';
import { useSyncExternalStore } from 'react';
import type * as THREE from 'three';
import { jobRequest } from './actions';
import { instanceMatrix } from './plater/geometry';
import { effectiveConfig, getState, subscribeStore, type PlateObject, type State } from './state/store';
import { plateSignature } from './slice-input';
import { styleOf } from './style-names';
import { slotFilaments } from './profiles/slots';

type Ring = [number, number][];
interface Footprint { contour: Ring; holes: Ring[] }
interface Summary { up_area_mm2: number; flat_area_mm2: number; curved_area_mm2: number | null; overhang_curved_area_mm2: number | null }
interface RawInstance {
  index: number;
  band_triangles: Record<string, string>;
  patches?: { kind: string; area_mm2: number; z: [number, number]; footprint: Footprint[]; lines: string }[];
  planar_areas?: { reason: string; area_mm2: number; footprint: Footprint[] }[];
}
interface RawPreview { schema: string; partial: boolean; error?: string; head: unknown; objects: { index: number; name: string; summary: Summary; instances: RawInstance[] }[] }

export interface InstanceOverlay {
  objectId: number;
  instanceId: number;
  at: THREE.Matrix4;
  shape: string;
  top: Float32Array;
  ledges: Float32Array;
  planar: { reason: string; footprint: Footprint[] }[];
}
export interface PreviewFact { objectId: number; name: string; curved: number; ledge: boolean }
export interface PatchPreview { key: string; overlays: InstanceOverlay[]; facts: PreviewFact[] }

const decode = (b64: string) => {
  const bytes = Uint8Array.from(atob(b64), (c) => c.charCodeAt(0));
  return new Float32Array(bytes.buffer, 0, bytes.byteLength >> 2);
};

function inRing(r: Ring, x: number, y: number) {
  let inside = false;
  for (let i = 0, j = r.length - 1; i < r.length; j = i++)
    if (r[i][1] > y !== r[j][1] > y && x < ((r[j][0] - r[i][0]) * (y - r[i][1])) / (r[j][1] - r[i][1]) + r[i][0]) inside = !inside;
  return inside;
}
export const inFootprint = (f: Footprint[], x: number, y: number) => f.some((p) => inRing(p.contour, x, y) && !p.holes.some((h) => inRing(h, x, y)));

function topTriangles(inst: RawInstance): Float32Array {
  const tops = (inst.patches ?? []).filter((p) => p.kind === 'top').map((p) => {
    const pts = p.footprint.flatMap((f) => f.contour);
    const xs = pts.map((q) => q[0]), ys = pts.map((q) => q[1]);
    return { p, box: [Math.min(...xs), Math.min(...ys), Math.max(...xs), Math.max(...ys)] };
  });
  if (!tops.length) return new Float32Array(0);
  const out: number[] = [];
  for (const b64 of Object.values(inst.band_triangles)) {
    const t = decode(b64);
    for (let i = 0; i + 9 <= t.length; i += 9) {
      const x = (t[i] + t[i + 3] + t[i + 6]) / 3, y = (t[i + 1] + t[i + 4] + t[i + 7]) / 3, z = (t[i + 2] + t[i + 5] + t[i + 8]) / 3;
      if (tops.some(({ p, box }) => x >= box[0] && x <= box[2] && y >= box[1] && y <= box[3] && z >= p.z[0] - 0.6 && z <= p.z[1] + 0.6 && inFootprint(p.footprint, x, y)))
        for (let k = 0; k < 9; k++) out.push(t[i + k]);
    }
  }
  return new Float32Array(out);
}

function ledgeLines(inst: RawInstance): Float32Array {
  const seen = new Set<string>();
  const parts: Float32Array[] = [];
  for (const p of inst.patches ?? []) {
    if (p.kind !== 'overhang' && p.kind !== 'bridge') continue;
    const k = JSON.stringify(p.footprint);
    if (seen.has(k)) continue;
    seen.add(k);
    parts.push(decode(p.lines));
  }
  const out = new Float32Array(parts.reduce((n, a) => n + a.length, 0));
  parts.reduce((o, a) => (out.set(a, o), o + a.length), 0);
  return out;
}

const REASONS: Record<string, string> = {
  steep: 'Too steep', steep_strip: 'Too steep', narrow: 'Too narrow', head: 'Print head would touch', flat: 'Already flat',
  flat_ground: 'Already flat', flat_sliver: 'Already flat', edge: 'Too close to edge', near_bed: 'Too close to bed',
  outline: 'Ragged outline', bottom: 'Faces down',
};
export const planarReason = (tag: string) => REASONS[tag] ?? (tag.startsWith('overhang') ? '' : 'Kept flat');

const shapeKey = (o: PlateObject) => `${o.rotX},${o.rotY},${o.scale}`;

function read(raw: RawPreview, objects: PlateObject[], key: string): PatchPreview {
  const overlays: InstanceOverlay[] = [];
  const facts: PreviewFact[] = [];
  for (const ro of raw.objects) {
    const o = objects[ro.index];
    if (!o) continue;
    const first = ro.instances[0];
    const curved = (first?.patches ?? []).filter((p) => p.kind === 'top').reduce((a, p) => a + p.area_mm2, 0);
    const flat = (first?.planar_areas ?? []).filter((a) => !a.reason.startsWith('overhang') && a.reason !== 'bottom' && a.area_mm2 >= 0.05).reduce((a, p) => a + p.area_mm2, 0);
    const s = ro.summary;
    facts.push({ objectId: o.id, name: o.name, curved: curved >= 1 ? Math.min(flat > 0 ? 99 : 100, Math.max(1, Math.round((100 * curved) / (curved + flat)))) : 0,
      ledge: (s.overhang_curved_area_mm2 ?? 0) > (s.curved_area_mm2 ?? 0) });
    for (const ri of ro.instances) {
      const inst = o.instances[ri.index];
      if (!inst) continue;
      overlays.push({ objectId: o.id, instanceId: inst.id, at: instanceMatrix(o, inst), shape: shapeKey(o), top: topTriangles(ri), ledges: ledgeLines(ri),
        planar: (ri.planar_areas ?? []).filter((a) => a.footprint.length) });
    }
  }
  return { key, overlays, facts };
}

let current: PatchPreview | null = null;
let shown = true;

const listeners = new Set<() => void>();
const emit = () => listeners.forEach((l) => l());
const subscribe = (l: () => void) => (listeners.add(l), () => void listeners.delete(l));
export const getPatchPreview = () => current;
export const subscribePatchPreview = subscribe;
export const usePatchPreview = () => useSyncExternalStore(subscribe, () => current);
export const useNonplanarView = () => useSyncExternalStore(subscribe, () => shown);
export const nonplanarViewShown = () => shown;
export function toggleNonplanarView() {
  shown = !shown;
  savePreferences({'orca.web.nonplanar_view':shown ? '1' : '0'});
  emit();
}

observePreference('orca.web.nonplanar_view', () => { shown = preferenceText('orca.web.nonplanar_view') !== '0'; emit(); });

export const overlayFits = (ov: InstanceOverlay, o: PlateObject) => ov.shape === shapeKey(o);


function wanted(s: State) {
  return !!s.api && !s.api.mock && s.tab === 'prepare' && s.objects.length > 0 && !s.project && styleOf(effectiveConfig(s, 'process')) === 'smooth';
}

const kept = new Map<string, PatchPreview>();
let timer: ReturnType<typeof setTimeout> | undefined;
let lastKey = '';
let lastInputs: unknown[] = [];
let running: { key: string; abort: AbortController; id?: string; stop?: () => void } | null = null;

function stopRunning() {
  const r = running;
  running = null;
  if (!r) return;
  r.abort.abort();
  r.stop?.();
  if (r.id) void getState().api?.cancel(r.id).catch(() => undefined);
}

async function request(key: string, budget: number) {
  const s = getState();
  if (!s.api) return;
  stopRunning();
  const mine = { key, abort: new AbortController() } as NonNullable<typeof running>;
  running = mine;
  const objects = s.objects;
  try {
    const req = { ...(await jobRequest(s)), events: [], output: 'gcode' as const, preview_budget: budget };
    const id = await s.api.submit(req, mine.abort.signal);
    if (running !== mine) return void s.api.cancel(id).catch(() => undefined);
    mine.id = id;
    mine.stop = s.api.events(id, {
      progress: () => undefined,
      done: (status) => {
        if (running !== mine) return;
        running = null;
        const raw = status.record as unknown as RawPreview | undefined;
        if (!raw || raw.schema !== 'orca.nonplanar_patch_preview.v1' || raw.error || !raw.head) return;
        if (raw.partial) return void (budget < 10 && request(key, 10));
        const p = read(raw, objects, key);
        kept.set(key, p);
        if (kept.size > 16) kept.delete(kept.keys().next().value!);
        if (lastKey === key) (current = p), emit();
      },
      error: () => void (running === mine && (running = null)),
    });
  } catch {
    if (running === mine) running = null;
  }
}

function sync() {
  const s = getState();
  if (!wanted(s)) {
    clearTimeout(timer);
    if (s.tab !== 'prepare') return;
    lastKey = '';
    stopRunning();
    if (current) (current = null), emit();
    return;
  }
  const inputs = [s.objects, s.dirty, s.printer, s.process, slotFilaments(s), s.filamentEdits, s.projectPresets, s.records, s.resolved, s.events];
  if (lastInputs.length && inputs.every((v, i) => v === lastInputs[i]) && lastKey) return;
  lastInputs = inputs;
  const key = plateSignature(s);
  if (key === lastKey) return;
  lastKey = key;
  clearTimeout(timer);
  const k = kept.get(key);
  if (k) {
    current = k;
    emit();
    return;
  }
  timer = setTimeout(() => void request(key, 2), 600);
}
subscribeStore(sync);
(window as unknown as { __patchPreview: unknown }).__patchPreview = () => current;
