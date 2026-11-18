import type { Config, PresetType } from '../api/types';
import { Dropdown } from '../components/Dropdown';
import { extruderLabel, installedVariant, type ExtruderSelection } from './config';
import type { PrinterRecord } from '../profiles/records';
import { filament_options_with_variant, printer_extruder_options, printer_options_with_variant_1, print_options_with_variant } from './variant-keys';
import { useEffect } from 'react';

export const perExtruderKeys = (type: PresetType): ReadonlySet<string> => type === 'process' ? print_options_with_variant : type === 'filament' ? filament_options_with_variant
  : new Set([...printer_options_with_variant_1, ...printer_extruder_options]);
export function useExtruderSelection(type: PresetType, config: Config, printer: PrinterRecord | undefined, selection: ExtruderSelection, onChange: (value: ExtruderSelection) => void) {
  const count = Math.max(1, printer?.nozzles.length ?? 1), extruder = Math.min(selection.extruder, count - 1), variant = installedVariant(config, type, printer?.nozzles[extruder], extruder);
  useEffect(() => { if (selection.extruder !== extruder || selection.variant !== variant) onChange({ extruder, variant }); });
}
export function ExtruderSwitch({ type, config, printer, selection, onChange }: {
  type: PresetType; config: Config; printer: PrinterRecord | undefined; selection: ExtruderSelection; onChange(value: ExtruderSelection): void;
}) {
  const count = printer?.nozzles.length ?? 0;
  if (!printer || count < 2) return null;
  const installed = printer.nozzles.map((n, e) => installedVariant(config, type, n, e)), bambu = printer.vendor === 'BBL';
  return <Dropdown className="combo compact extruder-switch" aria-label="Values for" value={String(Math.min(selection.extruder, count - 1))}
    options={installed.map((_, e) => ({ value: String(e), label: extruderLabel(e, count, bambu).replace(/ nozzle$/, '') }))}
    onValueChange={(id) => onChange({ extruder: Number(id), variant: installed[Number(id)] })}/>;
}
