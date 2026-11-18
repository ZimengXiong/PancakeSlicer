import type { Config } from '../api/types';
import rules from '../../../server/profile_rules.json';
export interface SpeedModel { schema: 2; multipliers: Record<string, number>; outerWallCeiling: number }
export interface SpeedDetail { base: number; multiplier: number; requested: number; speed: number;  limit: string; override?: string;  unset?: number; flowCap?: number; printerMax?: number; width: number; height: number; acceleration: number }
export const SPEED_FEATURES = rules.speedFeatures;
export const SPEED_KEYS = new Set(SPEED_FEATURES.map(f => f.key));
const seeds: Record<string, { multipliers: Record<string, number>; outerWallCeiling: number }> = rules.speedIntents;
const first = (value: Config[string] | undefined) => String(Array.isArray(value) ? value[0] ?? '' : value ?? '').split(',')[0];
const num = (config: Config, key: string, relative = 0) => { const t = first(config[key]); return t.endsWith('%') ? (Number(t.slice(0, -1)) || 0) * relative / 100 : Number(t) || 0; };
const minimum = (config: Config, ...keys: string[]) => Math.min(Infinity, ...keys.map(k => num(config, k)).filter(n => n > 0));
export function newSpeedModel(seed = 'standard'): SpeedModel {
  const preset = seeds[seed];
  if (!preset) throw new Error('Unrecognized speed template: ' + seed);
  return { schema: 2, multipliers: Object.fromEntries(SPEED_FEATURES.map(f => [f.key, preset.multipliers[f.key] ?? 1])), outerWallCeiling: preset.outerWallCeiling };
}
export function speedModelOf(profile: unknown): SpeedModel | undefined {
  return (profile as { auto?: { speedModel?: SpeedModel } } | undefined)?.auto?.speedModel;
}
export function resolveDynamicSpeeds(model: SpeedModel | undefined, base: Config, config: Config, overrides: Config, overrideSources: Record<string, string>, nozzle: number, height: number): { values: Config; details: Record<string, SpeedDetail> } {
  const values: Config = {}, details: Record<string, SpeedDetail> = {};
  if (!model) return { values, details };
  const printerMax = minimum(config, 'machine_max_speed_x', 'machine_max_speed_y');
  const printAccel = minimum(config, 'machine_max_acceleration_x', 'machine_max_acceleration_y', 'machine_max_acceleration_extruding');
  const travelAccel = minimum(config, 'machine_max_acceleration_x', 'machine_max_acceleration_y', 'machine_max_acceleration_travel');
  const flow = Math.min(Infinity, ...[config.filament_max_volumetric_speed ?? []].flat().flatMap(v => String(v).split(',')).map(Number).filter(n => n > 0));
  for (const f of SPEED_FEATURES) {
    const parent = 'relativeTo' in f && f.relativeTo ? f.relativeTo : f.key === 'support_interface_speed' ? 'support_speed' : 'sparse_infill_speed';
    const baseSpeed = num(base, f.key, num(base, parent));
    if (!(baseSpeed > 0)) continue;
    const multiplier = model.multipliers[f.key];
    const override = Object.hasOwn(overrides, f.key) ? overrideSources[f.key] : undefined;
    const requested = Object.hasOwn(overrides, f.key) ? num(overrides, f.key, 'relativeTo' in f && f.relativeTo ? num(values, f.relativeTo) || num(base, f.relativeTo) : baseSpeed) : baseSpeed * multiplier;
    let speed = requested, limit = '', width = 0, featureHeight = height, flowCap = Infinity;
    if (f.width) {
      if ('firstLayer' in f && f.firstLayer) featureHeight = num(config, 'initial_layer_print_height', height) || height;
      width = num(config, f.width, nozzle);
      if (f.width === 'bridge') { width = nozzle * Math.sqrt(num(config, 'bridge_flow', 1) || 1); featureHeight = width; }
      width ||= num(config, 'line_width', nozzle) || nozzle;
      if (width > 0 && featureHeight > 0) flowCap = flow / (width * featureHeight);
    }
    for (const [cap, name] of [[flowCap, 'Nozzle flow'], [printerMax, 'Printer max']] as const) if (cap < speed) { speed = cap; limit = name; }
    if (f.key === 'outer_wall_speed' && model.outerWallCeiling < speed) { speed = model.outerWallCeiling; limit = 'Outer-wall ceiling'; }
    let acceleration = num(config, f.acceleration, num(config, 'default_acceleration')) || num(config, 'default_acceleration');
    if (acceleration > 0) { acceleration = Math.min(acceleration, f.width ? printAccel : travelAccel); values[f.acceleration] = String(acceleration); }
    speed = Math.floor(speed * 1000 + 1e-7) / 1000;
    values[f.key] = String(speed);
    details[f.key] = { base: baseSpeed, multiplier, requested, speed, limit, ...(override ? { override } : {}), ...(Number.isFinite(flowCap) ? { flowCap } : {}), ...(Number.isFinite(printerMax) ? { printerMax } : {}), width, height: featureHeight, acceleration };
  }
  return { values, details };
}
