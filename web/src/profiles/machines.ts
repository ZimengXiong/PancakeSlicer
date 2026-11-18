import type { PrinterPreset } from '../state/store';



export function filamentSlots(printer?: PrinterPreset): number {
  if (!printer) return 1;
  const c = printer.config;
  const text = (k: string) => [c[k]].flat().join('\n');
  const extruders = Math.max(1, [c.nozzle_diameter].flat().length);
  const ams = /\bM620\b/.test(text('change_filament_gcode')) ? 4 : 1;
  const sem = [c.single_extruder_multi_material].flat()[0];
  const mmu = (sem === 1 || sem === '1' || sem === true) && /MMU|\bM702\b/i.test(`${printer.model ?? ''} ${printer.name} ${text('machine_start_gcode')} ${text('change_filament_gcode')}`) ? 5 : 1;
  return Math.max(extruders, ams, mmu);
}
