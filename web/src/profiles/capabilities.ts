import table from '../../../server/method_capabilities.json';
import type { Config } from '../api/types';
import { DEMO } from '../demo/config';

export interface MethodArtifact {
  validated?: boolean; preview_ready?: boolean; print_qualified?: boolean;
  binary_sha256?: string; resources_sha256?: string;
  features?: { curved_tops?: boolean; nonplanar_wave?: boolean };
  wave_modes?: readonly string[];
}
export interface MethodContext {
  printer: { known: boolean; config: Config; axes?: readonly string[] };
  effectiveConfig?: Config;
  engine: { artifact_manifest?: MethodArtifact };
}
export type MethodRecovery = 'change_method' | 'repair_printer' | 'update_engine';
export interface MethodAvailability {
  id: string; target: 'mode' | 'wave'; supported: boolean; reason?: string; recovery?: MethodRecovery;
}
export interface MethodAvailabilityResult {
  methods: MethodAvailability[]; allowedModes: string[]; allowedWaveModes: string[];
  printer: { known: boolean; kinematics: string; axes: number; nozzleKnown: boolean; rotary: boolean };
}
export interface MethodProblem { code: 'mode_incompatible'; method: string; reason: string; recovery: MethodRecovery }

function nozzleKnown(value: unknown): boolean {
  const values = Array.isArray(value) ? value : typeof value === 'string' ? value.split(/[,;]/) : [value];
  return values.length > 0 && values.every(v => (typeof v === 'number' || typeof v === 'string' && v.trim() !== '') && Number.isFinite(Number(v)) && Number(v) > 0);
}
export function deriveMethodAvailability(context: MethodContext): MethodAvailabilityResult {
  const config = context.printer.config, engine = context.engine, artifact = engine.artifact_manifest;
  const kinematics = String(config.nonplanar_kinematics ?? 'unknown');
  const expected = (table.kinematicsAxes as Record<string, number>)[kinematics] ?? 0;
  const stated = context.printer.axes ? new Set(context.printer.axes.map(a => a.toUpperCase())) : undefined;
  const axes = stated ? ['X', 'Y', 'Z'].every(a => stated.has(a)) ? stated.size : 0 : expected;
  const required = (table.kinematicsAxisNames as Record<string, string[]>)[kinematics] ?? [];
  const axesMatch = !stated || required.every(axis => stated.has(axis));
  const nozzle = nozzleKnown(config.nozzle_diameter);
  const known = context.printer.known && config.printer_technology === 'FFF' && expected > 0 && axes >= expected && axesMatch && nozzle;
  const rotary = known && kinematics !== 'none' && axes >= 4;
  const verified = (artifact?.preview_ready ?? artifact?.validated) === true
    && /^[a-f0-9]{64}$/i.test(artifact?.binary_sha256 ?? '') && /^[a-f0-9]{64}$/i.test(artifact?.resources_sha256 ?? '');
  const methods: MethodAvailability[] = table.methods.map(rule => {
    let reason: string | undefined, recovery: MethodRecovery = 'change_method';
    if (rule.id !== 'off') {
      const disabled = (rule as { disabled?: string }).disabled;
      if (disabled) reason = disabled;
      else if (!known) { reason = 'Check your printer\'s axes, kinematics, and nozzle settings before using this slicing method.'; recovery = 'repair_printer'; }
      else if (rule.hardware === 'rotary' && !rotary) reason = 'This slicing method requires a printer with rotary axes configured.';
      else if (DEMO) {  }
      else if (rule.readiness && (!verified || artifact?.features?.[rule.readiness as 'curved_tops' | 'nonplanar_wave'] !== true
        || rule.target === 'wave' && !artifact?.wave_modes?.includes(rule.id))) { reason = 'This slicing method isn\'t available in the current engine build.'; recovery = 'update_engine'; }
    }
    return { id: rule.id, target: rule.target as 'mode' | 'wave', supported: !reason, ...(reason ? { reason, recovery } : {}) };
  });
  return { methods, allowedModes: methods.filter(m => m.target === 'mode' && m.supported).map(m => m.id),
    allowedWaveModes: methods.filter(m => m.target === 'wave' && m.supported).map(m => m.id),
    printer: { known, kinematics, axes, nozzleKnown: nozzle, rotary } };
}
export function methodSelectionProblem(result: MethodAvailabilityResult, config: Config): MethodProblem | undefined {
  for (const [target, id] of [['mode', String(config.nonplanar_mode ?? 'off')], ['wave', String(config.nonplanar_wave ?? 'off')]] as const) {
    if (id === 'off') continue;
    const method = result.methods.find(m => m.target === target && m.id === id);
    if (!method?.supported) return { code: 'mode_incompatible', method: id,
      reason: method?.reason ?? 'The slicing method saved in this project isn\'t supported by your current printer and engine.', recovery: method?.recovery ?? 'change_method' };
  }
}
export const isRetiredSetting = (key: string) => key.startsWith('nonplanar_oneshot_');
export function isMethodSettingSupported(key: string, config: Config, result: MethodAvailabilityResult): boolean {
  if (key === 'nonplanar_mode') return true;
  if (isRetiredSetting(key)) return false;
  if (key === 'nonplanar_wave') return result.allowedWaveModes.length > 0;
  if (key.startsWith('rotary_')) return result.printer.rotary;
  if (key.startsWith('nonplanar_wave_')) return result.allowedWaveModes.includes(String(config.nonplanar_wave ?? 'off'));
  for (const rule of table.methods) if (rule.settingPrefixes?.some(prefix => key.startsWith(prefix))) {
    return config.nonplanar_mode === rule.id && result.allowedModes.includes(rule.id);
  }
  return true;
}
