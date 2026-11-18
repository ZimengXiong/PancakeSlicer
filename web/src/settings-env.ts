import type { PresetType } from './api/types';
import { OPTIONS, splitKey, type OptionMode, type TabLine } from './options';
import { effectiveConfig, resolvePreset, type State } from './state/store';
import { environment, type Environment } from './visibility';
import { currentPlate } from './settings/common';

export function tabEnvironment(state: State, type: PresetType, slot: number, mode: OptionMode): Environment {
  const machine = effectiveConfig(state, 'machine');
  const nozzles = machine.nozzle_diameter;
  return environment([machine, effectiveConfig(state, 'process'), effectiveConfig(state, type, slot)], {
    printer_vendor: resolvePreset(state, 'machine', state.printer).base?.vendor,
    extruders_count: Array.isArray(nozzles) ? nozzles.length : String(nozzles ?? '0.4').split(',').length,
    curr_bed_type: currentPlate(state), settings_scope: 'global',
    native_support_capable: nativeSupportCapable(state),
  }, mode);
}

export const nativeSupportCapable = (state: State) => !!resolvePreset(state, 'machine', state.printer).base?.native_support_capable;

export function lineMode(line: TabLine, k: string): OptionMode | undefined {
  const key = splitKey(k)[0];
  const mode = line.overrides?.[key]?.mode as string | undefined;
  return mode === 'Simple' ? 'simple' : mode === 'Advanced' ? 'advanced' : mode === 'Develop' ? 'developer' : mode ? 'develop' : OPTIONS[key]?.mode;
}
