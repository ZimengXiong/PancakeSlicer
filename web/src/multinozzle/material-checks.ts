import { filamentIds, hardness, modelIds, rules, type MaterialRule } from 'virtual:bambu-checks';
import type { PrinterStatus, Tray } from '../printers/connections';
import { extruderLabel } from './config';
import type { PrinterRecord } from '../profiles/records';
import { isExternalTray, type SendFilament } from './ams';

export const bambuFilamentId = (id: string) => filamentIds[id] ?? id;
export interface MaterialContext {
  model: string; name: string; type: string; vendor: string; id?: string; slot: 'ams' | 'ext';
  extruder?: number; flow?: string; diameter?: number; hasFilamentSwitch?: boolean;
  support?: boolean; object?: boolean; calibMode?: string;
}
const lower = (s: string) => s.toLowerCase();

function matches(rule: MaterialRule, context: MaterialContext): boolean {
  const name = lower(context.name), type = lower(context.type), vendor = lower(context.vendor);
  if (rule.model_id?.length && !rule.model_id.includes(modelIds[context.model] ?? context.model)) return false;
  if (rule.nozzle_flows?.length && !rule.nozzle_flows.includes(context.flow ?? '')) return false;
  if (rule.nozzle_diameters?.length && (context.diameter === undefined || !rule.nozzle_diameters.some((d) => Math.abs(d - context.diameter!) < 0.001))) return false;
  if (rule.extruder_id?.length && (context.extruder === undefined || !rule.extruder_id.includes(context.extruder))) return false;
  if (rule.vendor && (lower(rule.vendor) === 'bambu lab' ? vendor !== 'bambu lab' : vendor === 'bambu lab')) return false;
  if (rule.calib_mode && lower(rule.calib_mode) !== lower(context.calibMode ?? '')) return false;
  if (rule.type && lower(rule.type) !== type) return false;
  if (rule.types?.length && !rule.types.some((t) => lower(t) === type)) return false;
  if (rule.type_suffix && !type.endsWith(lower(rule.type_suffix))) return false;
  if (rule.name && name !== lower(rule.name)) return false;
  if (rule.name_suffix && !name.endsWith(lower(rule.name_suffix))) return false;
  if (rule.slot && rule.slot !== context.slot) return false;
  if (rule.has_filament_switch !== undefined && rule.has_filament_switch !== context.hasFilamentSwitch) return false;
  if (rule.used_for_print_support !== undefined && rule.used_for_print_support !== context.support) return false;
  if (rule.used_for_print_object !== undefined && rule.used_for_print_object !== context.object) return false;
  if (rule.white_names?.some((n) => name.includes(lower(n)))) return false;
  if (context.id && rule.white_fila_ids?.includes(bambuFilamentId(context.id))) return false;
  return true;
}

export function materialChecks(context: MaterialContext): { message: string; blocked: boolean }[] {
  return rules.filter((rule) => matches(rule, context)).map((rule) => ({
    message: rule.description.replace(/%s/g, /filaments are hard and brittle/.test(rule.description) ? context.type : context.name || context.type),
    blocked: rule.action === 'prohibition',
  }));
}

export const nozzleHardness = (type: string) => hardness[type];

export function preprintMaterialChecks(printer: PrinterRecord, status: PrinterStatus | undefined, filaments: SendFilament[], mapping: number[], trays: Tray[]) {
  const count = printer.nozzles.length;
  return filaments.flatMap((f, i) => {
    const tray = trays.find((t) => t.id === mapping[i]);
    const extruder = count === 1 ? 0 : f.extruder ?? tray?.extruder;
    const nozzle = extruder === undefined ? undefined : printer.nozzles[extruder];
    const flow = f.flow ?? (nozzle && (nozzle.volume ?? 'Standard')), diameter = f.diameter ?? nozzle?.diameter;
    const rows = materialChecks({ model: printer.model, name: tray?.name || f.name?.replace(/^Bambu (?:Lab )?/, '') || f.type, type: f.type, vendor: f.vendor ?? '', id: f.id,
      slot: tray && !isExternalTray(tray.id) ? 'ams' : 'ext', extruder, flow, diameter, hasFilamentSwitch: f.hasFilamentSwitch, object: f.object, support: f.support });
    if (extruder !== undefined) {
      const label = extruderLabel(extruder, count, true), level = ((f.compatibility ?? 0) >>> (extruder * 3)) & 7;
      if (!((f.printable ?? 3) & (1 << extruder)) || level === 1) rows.push({ message: `${f.name ?? f.type} isn't supported by ${label.toLowerCase()}.`, blocked: true });
      else if (level === 2 || level === 3) rows.push({ message: `${f.name ?? f.type} might have ${level === 2 ? 'critical ' : ''}print quality issues with ${label.toLowerCase()}.`, blocked: false });
      const actual = status?.online ? status.nozzles?.filter((n) => !n.abnormal && n.extruder === extruder && (count === 1 || !n.legacy) &&
        Math.abs(n.diameter - (diameter ?? 0)) < 0.001 && (!n.flow || flow === 'Hybrid' || n.flow === flow)) ?? [] : [];
      if (f.requiredHRC && actual.length && actual.every((n) => n.type && nozzleHardness(n.type) !== undefined && nozzleHardness(n.type) < f.requiredHRC!))
        rows.push({ message: `${f.name ?? f.type} needs a nozzle hardness of at least ${f.requiredHRC} HRC, and your ${label.toLowerCase()} is softer.`, blocked: false });
    }
    return rows.map((row) => ({ ...row, message: `Filament ${i + 1}: ${row.message}` }));
  });
}
