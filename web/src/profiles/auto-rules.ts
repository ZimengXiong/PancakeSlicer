import type { Config } from '../api/types';
import rules from '../../../server/profile_rules.json';

export type ScaleBasis = 'nozzle' | 'layer' | 'perLayer';
export interface ScaleRow { key: string; basis: ScaleBasis; factor: number }
export interface ScaleContext { nozzle: number; layerHeight: number }
export interface AutoRecord { schema: 3; scales: ScaleRow[]; preferredHeight?: number; heightRule?: string; speedModel?: import('./speed-rules').SpeedModel }

export const DEFAULT_SCALES: ScaleRow[] = rules.defaultScales.map((r) => ({ ...r, basis: r.basis as ScaleBasis }));

const round = (x: number) => Math.round(x * rules.round) / rules.round;
const isWidth = (key: string) => key === 'line_width' || key.endsWith('_line_width');
export const clampHeight = (layerHeight: number, nozzle: number) => nozzle > 0 ? Math.min(layerHeight, round(nozzle * rules.maxLayerOfNozzle)) : layerHeight;
const [minWidth, maxWidth] = rules.lineWidthOfNozzle;

export function scaledValue(row: ScaleRow, ctx: ScaleContext): number {
  const h = clampHeight(ctx.layerHeight, ctx.nozzle);
  const value = row.basis === 'nozzle' ? row.factor * ctx.nozzle : row.basis === 'layer' ? row.factor * h : Math.ceil(row.factor / h - 1e-6);
  return row.basis === 'perLayer' ? value : round(isWidth(row.key) ? Math.min(maxWidth * ctx.nozzle, Math.max(minWidth * ctx.nozzle, value)) : value);
}
export function scaleValues(scales: readonly ScaleRow[], ctx: ScaleContext): Config {
  if (!(ctx.nozzle > 0) || !(ctx.layerHeight > 0)) return {};
  return Object.fromEntries(scales.map((row) => [row.key, String(scaledValue(row, ctx))]));
}
export function effectiveShells(config: Config, side: 'top' | 'bottom', layerHeight: number): { layers: number; effective: number; thickness: number; lifted: boolean } {
  const num = (v: Config[string] | undefined) => { const x = Number(Array.isArray(v) ? v[0] : v); return Number.isFinite(x) ? x : 0; };
  const layers = Math.max(0, Math.round(num(config[`${side}_shell_layers`]))), thickness = num(config[`${side}_shell_thickness`]);
  const first = side === 'bottom' ? num(config.initial_layer_print_height) : 0;
  const minimum = !(thickness > 0 && layerHeight > 0 && layers > 0) ? 0
    : first > 0 ? (thickness <= first + 1e-6 ? 1 : 1 + Math.ceil((thickness - first) / layerHeight - 1e-6)) : Math.ceil(thickness / layerHeight - 1e-6);
  return { layers, effective: Math.max(layers, minimum), thickness, lifted: minimum > layers };
}
export const scalesOf = (record: unknown): ScaleRow[] => {
  const scales = (record as { auto?: { scales?: unknown } } | undefined)?.auto?.scales;
  return Array.isArray(scales) ? scales.filter((r): r is ScaleRow => !!r && typeof r.key === 'string' && Number.isFinite(r.factor) && ['nozzle', 'layer', 'perLayer'].includes(r.basis)) : [];
};
export function ruleHeight(rule: string, heights: number[], nozzle: number): number | undefined {
  const r = (rules.heightRules as Record<string, { coarsestOfNozzle?: number; nearestOfNozzle?: number }>)[rule];
  if (!r || !heights.length) return undefined;
  if (r.coarsestOfNozzle) return heights.filter((h) => h <= round(r.coarsestOfNozzle! * nozzle) + 1e-6).pop() ?? heights[0];
  const target = (r.nearestOfNozzle ?? 0) * nozzle;
  return heights.reduce((best, h) => Math.abs(h - target) < Math.abs(best - target) - 1e-9 ? h : best, heights[0]);
}
export function heightsFor(nozzle: number, min?: number, max?: number): number[] {
  const { step, minOfNozzle, minFloor, maxOfNozzle } = rules.heights;
  const lo = min && min > 0 ? min : Math.max(minFloor, Math.round(nozzle * minOfNozzle * 100) / 100), hi = Math.min(max && max > 0 ? max : nozzle * maxOfNozzle, nozzle * rules.maxLayerOfNozzle);
  const out: number[] = [];
  for (let h = lo; h <= hi + 1e-6; h += step) out.push(Math.round(h * 100) / 100);
  return out.length ? out : [lo];
}
export function scaleAcross(scales: readonly ScaleRow[], nozzles: readonly number[], layerHeight: number): Config {
  const out = scaleValues(scales, { nozzle: nozzles[0], layerHeight });
  for (const d of nozzles.slice(1)) {
    const other = scaleValues(scales, { nozzle: d, layerHeight });
    for (const [k, v] of Object.entries(out)) {
      if (other[k] === v || String(v).endsWith('%')) continue;
      const a = Number(v), b = Number(other[k]);
      if (k === 'initial_layer_print_height') out[k] = String(Math.min(a, b));
      else if (isWidth(k) && Math.abs(a / nozzles[0] - b / d) < 1e-9) out[k] = `${round(a / nozzles[0] * 100)}%`;
    }
  }
  return out;
}
