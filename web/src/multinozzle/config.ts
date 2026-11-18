import type { Config, ConfigValue, PresetType } from '../api/types';
import { normalizeConfigValue } from '../config-values';
import type { NozzleRecord, PrinterRecord } from '../profiles/records';
import { filament_options_with_variant, printer_extruder_options, printer_options_with_variant_1, printer_options_with_variant_2, print_options_with_variant } from './variant-keys';

export const valuesOf = (value: ConfigValue | undefined): string[] => value === undefined ? [] : (Array.isArray(value) ? value : String(value).split(/[;,]/)).map(String);
export const extruderLabel = (index: number, count: number, bambu: boolean) => bambu && count === 2 ? `${index === 0 ? 'Left' : 'Right'} nozzle` : `Extruder ${index + 1}`;

export function valueAt(value: ConfigValue | undefined, index: number, fallback = ''): string {
  const values = valuesOf(value);
  return values[index] ?? values[0] ?? fallback;
}

export function replaceAt(value: ConfigValue | undefined, index: number, next: string, size = index + 1): string[] {
  return Array.from({ length: Math.max(size, valuesOf(value).length) }, (_, i) => i === index ? next : valueAt(value, i, next));
}

export interface ExtruderSelection { extruder: number; variant: number }
const prefix = (type: PresetType) => type === 'machine' ? 'printer' : type === 'process' ? 'print' : 'filament';

export function variantsOf(config: Config, type: PresetType): string[] {
  return valuesOf(config[`${prefix(type)}_extruder_variant`]);
}

export function installedVariant(config: Config, type: PresetType, nozzle: NozzleRecord | undefined, extruder: number): number {
  const name = `${nozzle?.type ?? 'Direct Drive'} ${nozzle?.volume ?? 'Standard'}`;
  const variants = variantsOf(config, type);
  const ids = valuesOf(config[type === 'machine' ? 'printer_extruder_id' : 'print_extruder_id']);
  const index = variants.findIndex((v, i) => v === name && (type === 'filament' || Number(ids[i] ?? 1) === extruder + 1));
  return index < 0 ? 0 : index;
}

export function splitFilamentConfig(config: Config, slot: number): Config {
  const indices = valuesOf(config.filament_self_index).flatMap((id, i) => Number(id) === slot + 1 ? [i] : []);
  return Object.fromEntries(Object.entries(config).map(([key, value]) => [key, !Array.isArray(value) ? value :
    filament_options_with_variant.has(key) && indices.length ? indices.map((i) => valueAt(value, i)) : [valueAt(value, slot)]]));
}

export function variantIndex(config: Config, type: PresetType, selection: ExtruderSelection): number {
  const variants = variantsOf(config, type);
  if (type === 'filament') return Math.min(selection.variant, Math.max(0, variants.length - 1));
  const ids = valuesOf(config[`${prefix(type)}_extruder_id`]);
  const choices = variants.map((_, i) => i).filter((i) => Number(ids[i] ?? 1) === selection.extruder + 1);
  return choices.includes(selection.variant) ? selection.variant : choices[0] ?? 0;
}

export function optionIndex(key: string, type: PresetType, config: Config, selection: ExtruderSelection, column = 0): number | null {
  const variant = variantIndex(config, type, selection);
  if (type === 'filament') return filament_options_with_variant.has(key) ? variant : null;
  if (type === 'process') return print_options_with_variant.has(key) ? variant : null;
  if (printer_options_with_variant_2.has(key)) return variant * 2 + column;
  if (printer_options_with_variant_1.has(key)) return variant;
  return printer_extruder_options.has(key) || key === 'extruder_offset' ? selection.extruder : null;
}

export function nozzleElements(key: string, type: PresetType, config: Config, printer: PrinterRecord | undefined): { index: number; label?: string }[] {
  const count = Math.max(1, printer?.nozzles.length ?? 1);
  if (optionIndex(key, type, config, { extruder: 0, variant: 0 }) === null) return [{ index: 0 }];
  return Array.from({ length: count }, (_, e) => ({
    index: optionIndex(key, type, config, { extruder: e, variant: installedVariant(config, type, printer?.nozzles[e], e) })!,
    label: count > 1 ? extruderLabel(e, count, printer?.vendor === 'BBL').replace(/ nozzle$/, '') : undefined,
  }));
}

export function selectedOptionValues(config: Config, type: PresetType, selection: ExtruderSelection): Config {
  return Object.fromEntries(Object.entries(config).map(([key, value]) => {
    const index = optionIndex(key, type, config, selection);
    return [key, index === null ? value : [valueAt(value, index)]];
  }));
}

export function filamentVectors(configs: Config[], defaults: Config): Config {
  const variants = configs.map((c) => variantsOf(c, 'filament'));
  const counts = variants.map((list) => Math.max(1, list.length));
  const out: Config = {};
  for (const key of new Set(configs.flatMap(Object.keys))) {
    const values = configs.map((c) => c[key] ?? defaults[key]);
    if (values.every((v) => v === undefined || Array.isArray(v) && !v.length)) continue;
    if (filament_options_with_variant.has(key)) {
      out[key] = values.flatMap((v, i) => Array.from({ length: counts[i] }, (_, j) => valueAt(v, j)));
    } else {
      const normalized = values.map((v) => v === undefined ? undefined : normalizeConfigValue(key, v));
      out[key] = normalized.some(Array.isArray) ? normalized.map((v) => Array.isArray(v) ? String(v[0] ?? '') : String(v ?? '')) : normalized[0]!;
    }
  }
  out.filament_extruder_variant = variants.flatMap((list) => list.length ? list : ['Direct Drive Standard']);
  out.filament_self_index = counts.flatMap((count, slot) => Array.from({ length: count }, () => slot + 1));
  return out;
}
