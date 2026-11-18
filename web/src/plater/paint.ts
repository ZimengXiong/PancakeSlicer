import type * as THREE from 'three';
import { T3 } from '../three-runtime';
import { setState, type State } from '../state/store';

export type PaintKind = 'support' | 'seam' | 'fuzzy' | 'color';
export const PAINT_KINDS = ['support', 'seam', 'fuzzy', 'color'] as const;
export type BrushTool = 'circle' | 'sphere' | 'triangle' | 'height' | 'fill' | 'gap';
export const EXTRUDERS_LIMIT = 16;
export type PaintNode = number | PaintNode[];
export type Painting = Map<number, PaintNode>;

const COLOURS = [null, [0.5, 1, 0.5], [1, 0.5, 0.5]] as const;
const MAX_DEPTH = 8;

export const triangleCount = (g: THREE.BufferGeometry) => (g.index ? g.index.count : g.attributes.position.count) / 3;
const corner = (g: THREE.BufferGeometry, t: number, k: number) => (g.index ? g.index.getX(3 * t + k) : 3 * t + k);
type Tri = [THREE.Vector3, THREE.Vector3, THREE.Vector3];
const vertices = (g: THREE.BufferGeometry, t: number): Tri =>
  [0, 1, 2].map((k) => new T3.Vector3().fromBufferAttribute(g.attributes.position, corner(g, t, k))) as Tri;
const mid = (a: THREE.Vector3, b: THREE.Vector3) => a.clone().add(b).multiplyScalar(0.5);
const children = ([a, b, c]: Tri): Tri[] => {
  const ab = mid(a, b), bc = mid(b, c), ca = mid(c, a);
  return [[a, ab, ca], [ab, b, bc], [bc, c, ca], [ab, bc, ca]];
};

export type Paintable = { geometry: THREE.BufferGeometry; paint?: Partial<Record<PaintKind, Painting>> };

export function paintOf(o: Paintable, kind: PaintKind): Painting | null {
  const p = o.paint?.[kind];
  if (!p || !p.size) return null;
  const T = triangleCount(o.geometry);
  for (const t of p.keys()) if (t >= T) return null;
  return p;
}

const neighbourCache = new WeakMap<THREE.BufferGeometry, Int32Array>();
function neighbours(g: THREE.BufferGeometry): Int32Array {
  let n = neighbourCache.get(g);
  if (n) return n;
  const T = triangleCount(g);
  n = new Int32Array(3 * T).fill(-1);
  const edges = new Map<string, number>();
  for (let t = 0; t < T; t++)
    for (let k = 0; k < 3; k++) {
      const a = corner(g, t, k), b = corner(g, t, (k + 1) % 3);
      const key = a < b ? `${a},${b}` : `${b},${a}`;
      const other = edges.get(key);
      if (other === undefined) edges.set(key, 3 * t + k);
      else { n[3 * t + k] = Math.floor(other / 3); n[other] = t; edges.delete(key); }
    }
  neighbourCache.set(g, n);
  return n;
}

const normalCache = new WeakMap<THREE.BufferGeometry, Float32Array>();
function faceNormals(g: THREE.BufferGeometry): Float32Array {
  let out = normalCache.get(g);
  if (out) return out;
  const T = triangleCount(g);
  out = new Float32Array(3 * T);
  for (let t = 0; t < T; t++) {
    const [a, b, c] = vertices(g, t);
    c.sub(b).cross(a.sub(b)).normalize();
    out[3 * t] = c.x, out[3 * t + 1] = c.y, out[3 * t + 2] = c.z;
  }
  normalCache.set(g, out);
  return out;
}
const normalOf = (g: THREE.BufferGeometry, t: number) => { const n = faceNormals(g); return new T3.Vector3(n[3 * t], n[3 * t + 1], n[3 * t + 2]); };

function grow(g: THREE.BufferGeometry, seed: number, accept: (t: number, from: number) => boolean): number[] {
  const n = neighbours(g);
  const seen = new Uint8Array(triangleCount(g));
  const out = [seed], stack = [seed];
  seen[seed] = 1;
  while (stack.length) {
    const t = stack.pop()!;
    for (let k = 0; k < 3; k++) {
      const u = n[3 * t + k];
      if (u < 0 || seen[u] || !accept(u, t)) continue;
      seen[u] = 1;
      out.push(u);
      stack.push(u);
    }
  }
  return out;
}

interface CursorShape {
  inside(p: THREE.Vector3): boolean;
  faces(n: THREE.Vector3): boolean;
  touches(tri: Tri): boolean;
}

class HeightCursor implements CursorShape {
  constructor(private readonly world: THREE.Matrix4, private readonly z0: number, private readonly height: number) {}
  private z = (p: THREE.Vector3) => p.clone().applyMatrix4(this.world).z;
  inside = (p: THREE.Vector3) => { const z = this.z(p); return z > this.z0 - 0.02 && z < this.z0 + this.height + 0.02; };
  faces = () => true;
  touches(tri: Tri) {
    const zs = tri.map(this.z);
    return !(zs.every((z) => z < this.z0 - 1e-5) || zs.every((z) => z > this.z0 + this.height + 1e-5));
  }
}

class Cursor implements CursorShape {
  private readonly d: THREE.Vector3 | null;
  constructor(readonly centre: THREE.Vector3, readonly radius: number, tool: 'circle' | 'sphere', view: THREE.Vector3) {
    this.d = tool === 'circle' ? view.clone().normalize() : null;
  }
  private flat(p: THREE.Vector3) {
    const v = p.clone().sub(this.centre);
    return this.d ? v.addScaledVector(this.d, -v.dot(this.d)) : v;
  }
  inside = (p: THREE.Vector3) => this.flat(p).lengthSq() <= this.radius * this.radius;
  faces = (n: THREE.Vector3) => !this.d || n.dot(this.d) < 0;
  touches([a, b, c]: Tri): boolean {
    const [pa, pb, pc] = [a, b, c].map((p) => this.flat(p));
    const r2 = this.radius * this.radius;
    const seg = (p: THREE.Vector3, q: THREE.Vector3) => {
      const e = q.clone().sub(p), t = Math.min(1, Math.max(0, -p.dot(e) / (e.lengthSq() || 1)));
      return p.clone().addScaledVector(e, t).lengthSq() <= r2;
    };
    if (seg(pa, pb) || seg(pb, pc) || seg(pc, pa)) return true;
    const n = pb.clone().sub(pa).cross(pc.clone().sub(pa));
    const s1 = pb.clone().sub(pa).cross(pa.clone().negate()).dot(n), s2 = pc.clone().sub(pb).cross(pb.clone().negate()).dot(n), s3 = pa.clone().sub(pc).cross(pc.clone().negate()).dot(n);
    return (s1 >= 0 && s2 >= 0 && s3 >= 0) || (s1 <= 0 && s2 <= 0 && s3 <= 0);
  }
}

function brushNode(node: PaintNode, tri: Tri, cursor: CursorShape, state: number, limit2: number, depth: number): PaintNode {
  const inside = tri.map(cursor.inside);
  if (inside.every(Boolean)) return state;
  if (!inside.some(Boolean) && !cursor.touches(tri)) return node;
  const long = Math.max(tri[0].distanceToSquared(tri[1]), tri[1].distanceToSquared(tri[2]), tri[2].distanceToSquared(tri[0]));
  if (typeof node === 'number' && (long <= limit2 || depth >= MAX_DEPTH)) {
    const c = tri[0].clone().add(tri[1]).add(tri[2]).multiplyScalar(1 / 3);
    return cursor.inside(c) ? state : node;
  }
  const kids = typeof node === 'number' ? [node, node, node, node] : node;
  const next = children(tri).map((t, i) => brushNode(kids[i], t, cursor, state, limit2, depth + 1));
  return next.every((k) => k === next[0] && typeof k === 'number') ? next[0] : next;
}

const sameNode = (a: PaintNode | undefined, b: PaintNode | undefined): boolean =>
  (a ?? 0) === (b ?? 0) || Array.isArray(a) && Array.isArray(b) && a.every((k, i) => sameNode(k, b[i]));

function update(painting: Painting | null, tris: number[], paint: (node: PaintNode, t: number) => PaintNode): Painting | null {
  let next: Painting | null = null;
  for (const t of tris) {
    const before = painting?.get(t) ?? 0;
    const after = paint(before, t);
    if (sameNode(before, after)) continue;
    next ??= new Map(painting ?? []);
    if (after === 0) next.delete(t); else next.set(t, after);
  }
  return next ? (next.size ? next : null) : painting;
}

export function brush(g: THREE.BufferGeometry, painting: Painting | null, hit: number, point: THREE.Vector3, radius: number,
  tool: 'circle' | 'sphere', view: THREE.Vector3, state: number, allow: (t: number) => boolean = () => true): Painting | null {
  const cursor = new Cursor(point, radius, tool, view);
  const limit = radius / 3, limit2 = limit * limit;
  const tris = grow(g, hit, (t) => cursor.faces(normalOf(g, t)) && cursor.touches(vertices(g, t)));
  return update(painting, tris.filter((t) => cursor.faces(normalOf(g, t)) && allow(t)), (node, t) => brushNode(node, vertices(g, t), cursor, state, limit2, 0));
}

export function heightRange(g: THREE.BufferGeometry, painting: Painting | null, hit: number, world: THREE.Matrix4, z0: number, height: number,
  state: number): Painting | null {
  const cursor = new HeightCursor(world, z0, height);
  const limit = 0.1 / world.getMaxScaleOnAxis(), limit2 = limit * limit;
  const tris = grow(g, hit, (t) => cursor.touches(vertices(g, t)));
  return update(painting, tris, (node, t) => brushNode(node, vertices(g, t), cursor, state, limit2, 0));
}

export const paintTriangle = (painting: Painting | null, hit: number, state: number) => update(painting, [hit], () => state);

export function overhangFacets(g: THREE.BufferGeometry, normalMatrix: THREE.Matrix3, deg: number): ((t: number) => boolean) | null {
  if (!(deg > 0)) return null;
  const nrm = faceNormals(g), limit = -Math.cos((deg * Math.PI) / 180), v = new T3.Vector3();
  return (t) => v.set(nrm[3 * t], nrm[3 * t + 1], nrm[3 * t + 2]).applyMatrix3(normalMatrix).normalize().z < limit;
}

export function overhangGeometry(g: THREE.BufferGeometry, allow: (t: number) => boolean): THREE.BufferGeometry {
  const pos: number[] = [];
  for (let t = 0, T = triangleCount(g); t < T; t++)
    if (allow(t)) for (const v of vertices(g, t)) pos.push(v.x, v.y, v.z);
  const out = new T3.BufferGeometry();
  out.setAttribute('position', new T3.Float32BufferAttribute(pos, 3));
  return out;
}

export function fill(g: THREE.BufferGeometry, painting: Painting | null, hit: number, angle: number, state: number,
  allow: (t: number) => boolean = () => true): Painting | null {
  const nrm = faceNormals(g), limit = Math.cos((angle * Math.PI) / 180) - 1e-9;
  if (!allow(hit)) return painting;
  const tris = grow(g, hit, (t, from) => allow(t) && nrm[3 * t] * nrm[3 * from] + nrm[3 * t + 1] * nrm[3 * from + 1] + nrm[3 * t + 2] * nrm[3 * from + 2] >= limit);
  return update(painting, tris, () => state);
}

function wholeState(n: PaintNode | undefined): number {
  if (n === undefined) return 0;
  if (!Array.isArray(n)) return n;
  const s = n.map(wholeState);
  return s.every((v) => v === s[0]) ? s[0] : NaN;
}

export function bucketFill(g: THREE.BufferGeometry, painting: Painting | null, hit: number, angle: number, state: number): Painting | null {
  const nrm = faceNormals(g), limit = Math.cos((angle * Math.PI) / 180) - 1e-9, seed = wholeState(painting?.get(hit));
  const tris = grow(g, hit, (t, from) => wholeState(painting?.get(t)) === seed &&
    (angle < 0 || nrm[3 * t] * nrm[3 * from] + nrm[3 * t + 1] * nrm[3 * from + 1] + nrm[3 * t + 2] * nrm[3 * from + 2] >= limit));
  return update(painting, tris, () => state);
}

export function gapFill(g: THREE.BufferGeometry, painting: Painting | null, area: number): Painting | null {
  if (!(area > 0)) return painting;
  type Leaf = { t: number; path: number[]; tri: Tri; state: number };
  const leaves: Leaf[] = [];
  const walk = (node: PaintNode, tri: Tri, t: number, path: number[]) => {
    if (Array.isArray(node)) children(tri).forEach((c, i) => walk(node[i], c, t, [...path, i]));
    else leaves.push({ t, path, tri, state: node });
  };
  for (let t = 0, T = triangleCount(g); t < T; t++) walk(painting?.get(t) ?? 0, vertices(g, t), t, []);
  const key = (v: THREE.Vector3) => `${Math.round(v.x * 1e4)},${Math.round(v.y * 1e4)},${Math.round(v.z * 1e4)}`;
  const at = new Map<string, number[]>();
  leaves.forEach((l, i) => l.tri.forEach((v) => { const k = key(v); const list = at.get(k); if (list) list.push(i); else at.set(k, [i]); }));
  const parent = leaves.map((_, i) => i);
  const find = (i: number): number => (parent[i] === i ? i : (parent[i] = find(parent[i])));
  for (const list of at.values()) for (const j of list.slice(1)) if (leaves[j].state === leaves[list[0]].state) parent[find(j)] = find(list[0]);
  const areaOf = new Map<number, number>();
  leaves.forEach((l, i) => { const [a, b, c] = l.tri; const r = find(i); areaOf.set(r, (areaOf.get(r) ?? 0) + b.clone().sub(a).cross(c.clone().sub(a)).length() / 2); });
  const next = new Map<number, number>();
  leaves.forEach((l, i) => {
    const r = find(i);
    if (areaOf.get(r)! >= area) return;
    for (const v of l.tri) for (const j of at.get(key(v))!) if (find(j) !== r) next.set(r, Math.min(next.get(r) ?? 3, leaves[j].state));
  });
  if (!next.size) return painting;
  const roots = new Map<number, { path: number[]; state: number }[]>();
  leaves.forEach((l, i) => { const s = next.get(find(i)); if (s !== undefined && s !== l.state) (roots.get(l.t) ?? roots.set(l.t, []).get(l.t)!).push({ path: l.path, state: s }); });
  const set = (node: PaintNode, path: number[], state: number): PaintNode => {
    if (!path.length) return state;
    const kids = (node as PaintNode[]).slice();
    kids[path[0]] = set(kids[path[0]], path.slice(1), state);
    return kids.every((k) => k === kids[0] && typeof k === 'number') ? kids[0] : kids;
  };
  return update(painting, [...roots.keys()], (node, t) => roots.get(t)!.reduce((n, c) => set(n, c.path, c.state), node));
}

const overlayCache = new WeakMap<Painting, { key: string; geometry: THREE.BufferGeometry }>();
export function overlayGeometry(g: THREE.BufferGeometry, painting: Painting, palette?: (readonly number[] | null)[]): THREE.BufferGeometry {
  const key = palette ? JSON.stringify(palette) : '';
  const cached = overlayCache.get(painting);
  if (cached?.key === key) return cached.geometry;
  let out: THREE.BufferGeometry;
  const pos: number[] = [], col: number[] = [];
  const emit = (node: PaintNode, tri: Tri) => {
    if (Array.isArray(node)) return children(tri).forEach((t, i) => emit(node[i], t));
    const colour = (palette ?? COLOURS)[node];
    if (!colour) return;
    for (const v of tri) pos.push(v.x, v.y, v.z), col.push(...colour);
  };
  for (const [t, node] of painting) emit(node, vertices(g, t));
  out = new T3.BufferGeometry();
  out.setAttribute('position', new T3.Float32BufferAttribute(pos, 3));
  out.setAttribute('color', new T3.Float32BufferAttribute(col, 3));
  overlayCache.set(painting, { key, geometry: out });
  return out;
}

export function paintString(node: PaintNode): string {
  const nibbles: number[] = [];
  const put = (n: PaintNode) => {
    if (Array.isArray(n)) { nibbles.push(0b0011); for (let i = 3; i >= 0; i--) put(n[i]); }
    else if (n < 3) nibbles.push(n << 2);
    else nibbles.push(0b1100, ...(n <= 17 ? [n - 3] : [0b1111, n - 18]));
  };
  put(node);
  return nibbles.reverse().map((v) => v.toString(16).toUpperCase()).join('');
}

export function parsePaintString(text: string): PaintNode | null {
  const nibbles = [...text].reverse().map((c) => parseInt(c, 16));
  let k = 0;
  const read = (): PaintNode | null => {
    const n = nibbles[k++];
    if (n === 0b0011) {
      const children: PaintNode[] = [];
      for (let i = 3; i >= 0; i--) { const c = read(); if (c === null) return null; children[i] = c; }
      return children;
    }
    if ((n & 0b11) !== 0) return null;
    if (n >> 2 < 3) return n >> 2;
    const e = nibbles[k++];
    if (e === undefined) return null;
    if (e < 0b1111) return e + 3;
    const f = nibbles[k++];
    return f === undefined ? null : f + 18;
  };
  const node = nibbles.some(Number.isNaN) ? null : read();
  return node !== null && k === nibbles.length ? node : null;
}

export function archivePainting(paint: Partial<Record<PaintKind, Record<string, string>>>): Partial<Record<PaintKind, Painting>> | null {
  const out: Partial<Record<PaintKind, Painting>> = {};
  for (const [kind, map] of Object.entries(paint) as [PaintKind, Record<string, string>][]) {
    const painting: Painting = new Map();
    for (const [t, text] of Object.entries(map)) {
      const node = parsePaintString(text);
      if (node === null) return null;
      if (node !== 0) painting.set(Number(t), node);
    }
    if (painting.size) out[kind] = painting;
  }
  return out;
}

export function projectPaint(o: Paintable, saved: Partial<Record<PaintKind, Record<string, string>>> = {}, partTris?: number[]) {
  const ranges = partTris ?? [triangleCount(o.geometry)];
  const edits: { volume?: number; paint: Record<string, Record<string, string>> }[] = [];
  for (let volume = 0, start = 0; volume < ranges.length; start += ranges[volume++]) {
    const end = start + ranges[volume], out: Record<string, Record<string, string>> = {};
    for (const kind of PAINT_KINDS) {
      const painting = paintOf(o, kind);
      const map: Record<string, string> = {}, was: Record<string, string> = {};
      if (painting) for (const [t, node] of [...painting].sort((x, y) => x[0] - y[0])) if (t >= start && t < end) map[String(t - start)] = paintString(node);
      for (const [t, v] of Object.entries(saved[kind] ?? {})) if (+t >= start && +t < end) was[String(+t - start)] = v;
      const same = Object.keys(map).length === Object.keys(was).length && Object.entries(map).every(([t, v]) => was[t] === v);
      if (!same) out[kind] = map;
    }
    if (Object.keys(out).length) edits.push({ ...(partTris ? { volume } : {}), paint: out });
  }
  return edits;
}

export function paintJson(o: Paintable): Record<string, Record<string, string>> | undefined {
  const out: Record<string, Record<string, string>> = {};
  for (const kind of PAINT_KINDS) {
    const painting = paintOf(o, kind);
    if (!painting) continue;
    const degenerate: number[] = [];
    for (let t = 0, T = triangleCount(o.geometry); t < T; t++) {
      const a = corner(o.geometry, t, 0), b = corner(o.geometry, t, 1), c = corner(o.geometry, t, 2);
      if (a === b || b === c || a === c) degenerate.push(t);
    }
    const before = (t: number) => { let lo = 0, hi = degenerate.length; while (lo < hi) { const m = (lo + hi) >> 1; if (degenerate[m] < t) lo = m + 1; else hi = m; } return lo; };
    const map: Record<string, string> = {};
    for (const [t, node] of [...painting].sort((x, y) => x[0] - y[0]))
      if (!degenerate.includes(t)) map[String(t - before(t))] = paintString(node);
    if (Object.keys(map).length) out[kind] = map;
  }
  return Object.keys(out).length ? out : undefined;
}

export interface PaintTool {
  tool: BrushTool; radius: number; angle: number;
  height: number; filament: number;
  gapArea: number; highlight: number; overhangsOnly: boolean;
  edges: boolean;
}
export const RADIUS = { min: 0.4, max: 8, step: 0.2 } as const;
export const ANGLE = { min: 0, max: 90, step: 5 } as const;
export const HEIGHT = { min: 0.1, max: 8, step: 0.2 } as const;
export const GAP_AREA = { min: 0, max: 5, step: 0.05 } as const;
let tool: PaintTool = { tool: 'circle', radius: 1, angle: 30, height: 1, filament: 1, gapArea: 0, highlight: 0, overhangsOnly: false, edges: false };
const listeners = new Set<() => void>();
export const paintTool = () => tool;
export function setPaintTool(patch: Partial<PaintTool>) {
  tool = { ...tool, ...patch };
  tool.radius = Math.min(RADIUS.max, Math.max(RADIUS.min, Math.round(tool.radius * 100) / 100));
  tool.angle = Math.min(ANGLE.max, Math.max(ANGLE.min, tool.angle));
  tool.gapArea = Math.min(GAP_AREA.max, Math.max(GAP_AREA.min, tool.gapArea));
  tool.highlight = Math.min(90, Math.max(0, tool.highlight));
  tool.height = Math.min(HEIGHT.max, Math.max(HEIGHT.min, Math.round(tool.height * 100) / 100));
  listeners.forEach((l) => l());
}
export const subscribePaintTool = (l: () => void) => (listeners.add(l), () => void listeners.delete(l));

export const canPaint = (s: State) => !!s.selected && !s.selected.all &&
  !!s.objects.find((o) => o.id === s.selected!.object && (!o.project ? !s.project : canPaintProject(o)));

const canPaintProject = (o: State['objects'][number]) => !!o.project?.paintable &&
  !o.project.placeholder && !o.volumes?.some((v) => v.type === 'part');

export const togglePaint = (kind: PaintKind) => setState((s) => ({ gizmo: s.gizmo === kind || !canPaint(s) ? 'none' : kind }));

export const isPaint = (gizmo: string): gizmo is PaintKind => (PAINT_KINDS as readonly string[]).includes(gizmo);

export const PAINT_STEP: Record<PaintKind, string> = {
  support: 'Paint-on supports editing', seam: 'Paint-on seam editing', fuzzy: 'Paint-on fuzzy skin editing', color: 'Color painting editing',
};

export const PAINT_TOOLS: Record<PaintKind, [BrushTool, string, string][]> = {
  support: [['circle', 'circle_paint', 'Circle'], ['sphere', 'toolbar_modifier_sphere', 'Sphere'], ['fill', 'fill_paint', 'Fill'], ['gap', 'gap_fill', 'Gap Fill']],
  seam: [['circle', 'circle_paint', 'Circle'], ['sphere', 'toolbar_modifier_sphere', 'Sphere']],
  fuzzy: [['circle', 'circle_paint', 'Circle'], ['sphere', 'toolbar_modifier_sphere', 'Sphere'], ['fill', 'fill_paint', 'Fill']],
  color: [['circle', 'circle_paint', 'Circle'], ['sphere', 'toolbar_modifier_sphere', 'Sphere'], ['triangle', 'triangle_paint', 'Triangle'],
    ['height', 'height_range', 'Height Range'], ['fill', 'fill_paint', 'Fill'], ['gap', 'gap_fill', 'Gap Fill']],
};

const PAINT_KEYS: Record<string, BrushTool> = { c: 'circle', s: 'sphere', f: 'fill', g: 'gap', t: 'triangle', h: 'height' };
export function paintKeyTool(kind: PaintKind, key: string): BrushTool | null {
  const t = PAINT_KEYS[key.toLowerCase()];
  return t && PAINT_TOOLS[kind].some(([x]) => x === t) ? t : null;
}
