import { toast, type PlateObject } from '../state/store';

export interface Simplified {
  trianglesBefore: number;
  trianglesAfter: number;
  maxErrorMm: number;
  kept: 'simplified' | 'original';
}

export const simplifiedOf = (o: PlateObject): Simplified | null => (o as PlateObject & { simplification?: Simplified }).simplification ?? null;

const pipeline = import.meta.glob<{ setKeepOriginal(id: number, keep: boolean): Promise<void> }>('../plater/simplify.ts');
export const setKeepOriginal = async (id: number, keep: boolean) => (await Object.values(pipeline)[0]?.())?.setKeepOriginal(id, keep);

export function count(n: number) {
  if (n >= 1e6) return `${(n / 1e6).toFixed(n >= 1e7 ? 0 : 1)}M`;
  if (n >= 1e3) return `${Math.round(n / 1e3)}k`;
  return String(n);
}

export function noteSimplified(list: Simplified[]) {
  const done = list.filter((s) => s.trianglesAfter < s.trianglesBefore);
  if (!done.length) return;
  const before = done.reduce((n, s) => n + s.trianglesBefore, 0), after = done.reduce((n, s) => n + s.trianglesAfter, 0);
  toast('simplified_triangles', { value: count(before), value2: count(after) });
}
