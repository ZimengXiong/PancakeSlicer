import { Vector3 } from 'three';
import { ConvexHull } from 'three/examples/jsm/math/ConvexHull.js';

export function parseStl(buf: ArrayBuffer): Float32Array {
  const dv = new DataView(buf);
  const tris = buf.byteLength >= 84 ? dv.getUint32(80, true) : -1;
  if (tris >= 0 && 84 + tris * 50 === buf.byteLength) {
    if (!tris) throw new Error('there are no triangles in it.');
    const out = new Float32Array(tris * 9);
    for (let t = 0, o = 0; t < tris; t++) {
      const base = 84 + t * 50 + 12;
      for (let k = 0; k < 9; k++) out[o++] = dv.getFloat32(base + 4 * k, true);
    }
    return out;
  }
  const text = new TextDecoder().decode(buf);
  const out: number[] = [];
  const re = /vertex\s+(\S+)\s+(\S+)\s+(\S+)/g;
  for (let m; (m = re.exec(text)); ) out.push(+m[1], +m[2], +m[3]);
  if (!out.length) throw new Error('there are no triangles in it.');
  return new Float32Array(out);
}

export function parseObj(text: string): Float32Array {
  const v: number[] = [];
  const out: number[] = [];
  const idx = (s: string) => {
    const i = parseInt(s, 10);
    return i < 0 ? v.length / 3 + i : i - 1;
  };
  const push = (i: number) => out.push(v[3 * i], v[3 * i + 1], v[3 * i + 2]);
  for (const line of text.split('\n')) {
    if (line.startsWith('v ')) {
      const p = line.trim().split(/\s+/);
      v.push(+p[1], +p[2], +p[3]);
    } else if (line.startsWith('f ')) {
      const p = line.trim().split(/\s+/).slice(1).map(idx);
      for (let k = 1; k + 1 < p.length; k++) push(p[0]), push(p[k]), push(p[k + 1]);
    }
  }
  if (!out.length) throw new Error('there\'s no 3D mesh in it.');
  return new Float32Array(out);
}

export function weld(soup: Float32Array): { positions: Float32Array; index: Uint32Array } {
  const n = soup.length / 3;
  let size = 1;
  while (size < n * 1.5) size <<= 1;
  const mask = size - 1;
  const table = new Int32Array(size).fill(-1);
  const keys = new Int32Array(n * 3);
  const positions = new Float32Array(n * 3);
  const index = new Uint32Array(n);
  let unique = 0;
  for (let i = 0; i < n; i++) {
    const x = soup[3 * i], y = soup[3 * i + 1], z = soup[3 * i + 2];
    const a = Math.trunc(x * 1e4 + 0.5), b = Math.trunc(y * 1e4 + 0.5), c = Math.trunc(z * 1e4 + 0.5);
    let h = (Math.imul(a, 73856093) ^ Math.imul(b, 19349663) ^ Math.imul(c, 83492791)) & mask;
    for (;;) {
      const j = table[h];
      if (j < 0) {
        table[h] = unique;
        keys[3 * unique] = a, keys[3 * unique + 1] = b, keys[3 * unique + 2] = c;
        positions[3 * unique] = x, positions[3 * unique + 1] = y, positions[3 * unique + 2] = z;
        index[i] = unique++;
        break;
      }
      if (keys[3 * j] === a && keys[3 * j + 1] === b && keys[3 * j + 2] === c) {
        index[i] = j;
        break;
      }
      h = (h + 1) & mask;
    }
  }
  return { positions: positions.slice(0, unique * 3), index };
}

export function vertexNormals(p: Float32Array, index: Uint32Array): Float32Array {
  const nrm = new Float32Array(p.length);
  for (let t = 0; t < index.length; t += 3) {
    const a = 3 * index[t], b = 3 * index[t + 1], c = 3 * index[t + 2];
    const cbx = p[c] - p[b], cby = p[c + 1] - p[b + 1], cbz = p[c + 2] - p[b + 2];
    const abx = p[a] - p[b], aby = p[a + 1] - p[b + 1], abz = p[a + 2] - p[b + 2];
    const nx = cby * abz - cbz * aby, ny = cbz * abx - cbx * abz, nz = cbx * aby - cby * abx;
    for (const v of [a, b, c]) nrm[v] += nx, nrm[v + 1] += ny, nrm[v + 2] += nz;
  }
  for (let i = 0; i < nrm.length; i += 3) {
    const l = Math.hypot(nrm[i], nrm[i + 1], nrm[i + 2]) || 1;
    nrm[i] /= l, nrm[i + 1] /= l, nrm[i + 2] /= l;
  }
  return nrm;
}

export function creaseSplit(p: Float32Array, index: Uint32Array, creaseDeg = 30, lineDeg = 40): { positions: Float32Array; normals: Float32Array; index: Uint32Array; edges: Float32Array } {
  const nt = index.length / 3, nv = p.length / 3;
  const limit = Math.cos(creaseDeg * Math.PI / 180), lineLimit = Math.cos(lineDeg * Math.PI / 180);
  const fn = new Float32Array(nt * 3), angle = new Float32Array(nt * 3);
  for (let t = 0; t < nt; t++) {
    const a = 3 * index[3 * t], b = 3 * index[3 * t + 1], c = 3 * index[3 * t + 2];
    const cbx = p[c] - p[b], cby = p[c + 1] - p[b + 1], cbz = p[c + 2] - p[b + 2];
    const abx = p[a] - p[b], aby = p[a + 1] - p[b + 1], abz = p[a + 2] - p[b + 2];
    const nx = cby * abz - cbz * aby, ny = cbz * abx - cbx * abz, nz = cbx * aby - cby * abx;
    const l = Math.hypot(nx, ny, nz);
    if (l > 0) fn[3 * t] = nx / l, fn[3 * t + 1] = ny / l, fn[3 * t + 2] = nz / l;
    for (let k = 0; k < 3; k++) {
      const o = 3 * index[3 * t + k], u = 3 * index[3 * t + (k + 1) % 3], w = 3 * index[3 * t + (k + 2) % 3];
      const ux = p[u] - p[o], uy = p[u + 1] - p[o + 1], uz = p[u + 2] - p[o + 2];
      const wx = p[w] - p[o], wy = p[w + 1] - p[o + 1], wz = p[w + 2] - p[o + 2];
      const d = Math.hypot(ux, uy, uz) * Math.hypot(wx, wy, wz);
      angle[3 * t + k] = d > 0 ? Math.acos(Math.max(-1, Math.min(1, (ux * wx + uy * wy + uz * wz) / d))) : 0;
    }
  }
  const start = new Uint32Array(nv + 1);
  for (let i = 0; i < index.length; i++) start[index[i] + 1]++;
  for (let v = 0; v < nv; v++) start[v + 1] += start[v];
  const fill = start.slice(0, nv), corners = new Uint32Array(index.length);
  for (let i = 0; i < index.length; i++) corners[fill[index[i]]++] = i;
  const positions = new Float32Array(index.length * 3), normals = new Float32Array(index.length * 3), out = new Uint32Array(index.length);
  let count = 0;
  for (let v = 0; v < nv; v++) {
    const first = count;
    for (let i = start[v]; i < start[v + 1]; i++) {
      const corner = corners[i], f = 3 * Math.floor(corner / 3);
      let x = 0, y = 0, z = 0;
      for (let j = start[v]; j < start[v + 1]; j++) {
        const g = corners[j], h = 3 * Math.floor(g / 3);
        if (fn[f] * fn[h] + fn[f + 1] * fn[h + 1] + fn[f + 2] * fn[h + 2] < limit) continue;
        x += angle[g] * fn[h], y += angle[g] * fn[h + 1], z += angle[g] * fn[h + 2];
      }
      const l = Math.hypot(x, y, z);
      if (l > 0) x /= l, y /= l, z /= l;
      else z = 1;
      let found = -1;
      for (let o = first; o < count && found < 0; o++)
        if (normals[3 * o] * x + normals[3 * o + 1] * y + normals[3 * o + 2] * z > 0.9999) found = o;
      if (found < 0) {
        found = count++;
        positions[3 * found] = p[3 * v], positions[3 * found + 1] = p[3 * v + 1], positions[3 * found + 2] = p[3 * v + 2];
        normals[3 * found] = x, normals[3 * found + 1] = y, normals[3 * found + 2] = z;
      }
      out[corner] = found;
    }
  }
  const ends: number[] = [];
  for (let v = 0; v < nv; v++)
    for (let i = start[v]; i < start[v + 1]; i++) {
      const corner = corners[i], t = Math.floor(corner / 3), w = index[3 * t + (corner % 3 + 1) % 3];
      let other = -1, shared = 0;
      for (let j = start[w]; j < start[w + 1]; j++) {
        const g = corners[j], u = Math.floor(g / 3);
        if (u !== t && index[3 * u + (g % 3 + 1) % 3] === v) other = u, shared++;
      }
      if (shared === 1 && v > w) continue;
      const open = shared !== 1;
      const f = 3 * t, h = 3 * other;
      const sliver = !(fn[f] || fn[f + 1] || fn[f + 2]) || !open && !(fn[h] || fn[h + 1] || fn[h + 2]);
      if (sliver || !open && fn[f] * fn[h] + fn[f + 1] * fn[h + 1] + fn[f + 2] * fn[h + 2] >= lineLimit) continue;
      ends.push(v, w);
    }
  return { positions: positions.slice(0, count * 3), normals: normals.slice(0, count * 3), index: out, edges: featureLines(p, ends) };
}

function featureLines(p: Float32Array, ends: number[]): Float32Array {
  const min = [Infinity, Infinity, Infinity], max = [-Infinity, -Infinity, -Infinity];
  for (let i = 0; i < p.length; i += 3) for (let k = 0; k < 3; k++) { min[k] = Math.min(min[k], p[i + k]); max[k] = Math.max(max[k], p[i + k]); }
  const shortest = 0.03 * Math.hypot(max[0] - min[0], max[1] - min[1], max[2] - min[2]);
  const parent = new Map<number, number>();
  const root = (v: number): number => {
    let r = v;
    while (parent.has(r) && parent.get(r) !== r) r = parent.get(r)!;
    for (let x = v; x !== r;) { const next = parent.get(x)!; parent.set(x, r); x = next; }
    return r;
  };
  for (let i = 0; i < ends.length; i += 2) {
    for (const v of [ends[i], ends[i + 1]]) if (!parent.has(v)) parent.set(v, v);
    const a = root(ends[i]), b = root(ends[i + 1]);
    if (a !== b) parent.set(a, b);
  }
  const length = (i: number) => Math.hypot(p[3 * ends[i]] - p[3 * ends[i + 1]], p[3 * ends[i] + 1] - p[3 * ends[i + 1] + 1], p[3 * ends[i] + 2] - p[3 * ends[i + 1] + 2]);
  const total = new Map<number, number>();
  for (let i = 0; i < ends.length; i += 2) { const r = root(ends[i]); total.set(r, (total.get(r) ?? 0) + length(i)); }
  const segments: number[] = [];
  for (let i = 0; i < ends.length; i += 2) {
    if ((total.get(root(ends[i])) ?? 0) < shortest) continue;
    const v = ends[i], w = ends[i + 1];
    segments.push(p[3 * v], p[3 * v + 1], p[3 * v + 2], p[3 * w], p[3 * w + 1], p[3 * w + 2]);
  }
  return new Float32Array(segments);
}

export function centre(p: Float32Array): { centre: [number, number, number]; min: [number, number, number]; max: [number, number, number] } {
  const min = [Infinity, Infinity, Infinity], max = [-Infinity, -Infinity, -Infinity];
  for (let i = 0; i < p.length; i += 3)
    for (let k = 0; k < 3; k++) {
      const v = p[i + k];
      if (v < min[k]) min[k] = v;
      if (v > max[k]) max[k] = v;
    }
  const c: [number, number, number] = [(min[0] + max[0]) / 2, (min[1] + max[1]) / 2, min[2]];
  for (let i = 0; i < p.length; i += 3) p[i] -= c[0], p[i + 1] -= c[1], p[i + 2] -= c[2];
  return { centre: c, min: [min[0] - c[0], min[1] - c[1], 0], max: [max[0] - c[0], max[1] - c[1], max[2] - c[2]] };
}

export function hullPoints(p: Float32Array): Float32Array {
  const n = p.length / 3;
  if (n <= 2048) return p.slice();
  const dirs: number[][] = [];
  for (let x = -1; x <= 1; x++) for (let y = -1; y <= 1; y++) for (let z = -1; z <= 1; z++) if (x || y || z) dirs.push([x, y, z]);
  const best = dirs.map(() => -Infinity), arg = dirs.map(() => 0);
  for (let i = 0; i < n; i++) {
    const x = p[3 * i], y = p[3 * i + 1], z = p[3 * i + 2];
    for (let d = 0; d < dirs.length; d++) {
      const s = dirs[d][0] * x + dirs[d][1] * y + dirs[d][2] * z;
      if (s > best[d]) best[d] = s, arg[d] = i;
    }
  }
  const vec = (i: number) => new Vector3(p[3 * i], p[3 * i + 1], p[3 * i + 2]);
  const inner = new ConvexHull().setFromPoints([...new Set(arg)].map(vec));
  const planes = inner.faces.map((f) => [f.normal.x, f.normal.y, f.normal.z, f.constant]);
  const keep: Vector3[] = [];
  const eps = 1e-6;
  for (let i = 0; i < n; i++) {
    const x = p[3 * i], y = p[3 * i + 1], z = p[3 * i + 2];
    let outside = planes.length < 4;
    for (let f = 0; f < planes.length && !outside; f++) outside = planes[f][0] * x + planes[f][1] * y + planes[f][2] * z - planes[f][3] > -eps;
    if (outside) keep.push(new Vector3(x, y, z));
  }
  const hull = new ConvexHull().setFromPoints(keep);
  const verts = new Set<Vector3>();
  for (const f of hull.faces) {
    let e = f.edge;
    do {
      verts.add(e.head().point);
      e = e.next;
    } while (e !== f.edge);
  }
  if (verts.size < 4) return p.slice();
  const out = new Float32Array(verts.size * 3);
  let o = 0;
  for (const v of verts) out[o++] = v.x, out[o++] = v.y, out[o++] = v.z;
  return out;
}

export function bakeStl(p: Float32Array, index: Uint32Array | Uint16Array | null, m: ArrayLike<number>): Blob {
  const tp = new Float64Array(p.length);
  for (let i = 0; i < p.length; i += 3) {
    const x = p[i], y = p[i + 1], z = p[i + 2];
    tp[i] = m[0] * x + m[4] * y + m[8] * z + m[12];
    tp[i + 1] = m[1] * x + m[5] * y + m[9] * z + m[13];
    tp[i + 2] = m[2] * x + m[6] * y + m[10] * z + m[14];
  }
  const tris = (index ? index.length : p.length / 3) / 3;
  const flip = m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) + m[8] * (m[1] * m[6] - m[5] * m[2]) < 0;
  const head = new ArrayBuffer(84);
  new TextEncoder().encodeInto('PancakeSlicer web plate object', new Uint8Array(head, 0, 80));
  new DataView(head).setUint32(80, tris, true);
  const parts: ArrayBuffer[] = [head];
  const PER = 20971;
  const rec = new Float32Array(13);
  const recBytes = new Uint8Array(rec.buffer, 0, 50);
  for (let t0 = 0; t0 < tris; t0 += PER) {
    const count = Math.min(PER, tris - t0);
    const out = new Uint8Array(count * 50);
    for (let t = t0, off = 0; t < t0 + count; t++, off += 50) {
      const a = 3 * (index ? index[3 * t] : 3 * t);
      let b = 3 * (index ? index[3 * t + 1] : 3 * t + 1), c = 3 * (index ? index[3 * t + 2] : 3 * t + 2);
      if (flip) [b, c] = [c, b];
      const cbx = tp[c] - tp[b], cby = tp[c + 1] - tp[b + 1], cbz = tp[c + 2] - tp[b + 2];
      const abx = tp[a] - tp[b], aby = tp[a + 1] - tp[b + 1], abz = tp[a + 2] - tp[b + 2];
      const nx = cby * abz - cbz * aby, ny = cbz * abx - cbx * abz, nz = cbx * aby - cby * abx;
      const l = Math.hypot(nx, ny, nz) || 1;
      rec[0] = nx / l, rec[1] = ny / l, rec[2] = nz / l;
      rec[3] = tp[a], rec[4] = tp[a + 1], rec[5] = tp[a + 2];
      rec[6] = tp[b], rec[7] = tp[b + 1], rec[8] = tp[b + 2];
      rec[9] = tp[c], rec[10] = tp[c + 1], rec[11] = tp[c + 2];
      out.set(recBytes, off);
    }
    parts.push(out.buffer);
  }
  return new Blob(parts, { type: 'model/stl' });
}
