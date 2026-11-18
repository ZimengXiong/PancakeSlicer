import type { PresetType } from '../api/types';
import { isHidden, modeVisible, OPTIONS, splitKey, TABS, type OptionMode, type TabGroup, type TabPage } from '../options';
import { effectiveConfig, effectiveMode, type State } from '../state/store';
import { plateSetup } from '../profiles/plate-setup';
import { lineMode, tabEnvironment } from '../settings-env';
import { condition, optionState, settingVisible } from '../visibility';
import common from '../tabs/common.json';

const TAB = { machine: 'printer', process: 'process', filament: 'filament' } as const;
const plates: Record<string, string> = common.plates;
const bedKeys = new Set(Object.values(plates).flatMap((k) => [`${k}_initial_layer`, k]));
const emptyRanges = new Set(common.emptyRanges);
const scalar = (v: unknown): unknown => Array.isArray(v) ? v[0] : v;
const empty = (v: unknown): boolean => v == null || typeof v === 'string' && /^(?:\s*|nil|n\/?a)$/i.test(v.trim())
  || typeof v === 'number' && !Number.isFinite(v) || Array.isArray(v) && (!v.length || v.every(empty))
  || typeof v === 'object' && v !== null && 'value' in v && empty(v.value);

export function currentPlate(s: State): string {
  return plateSetup(s)?.plate ?? '';
}

export function commonKeys(type: PresetType, s: State): Set<string> {
  const out = new Set<string>();
  for (const group of common[type].groups) {
    for (const key of group.keys) out.add(key);
    if (type === 'filament' && group.group === 'Bed temperature') {
      const bed = plates[currentPlate(s)];
      if (bed) { out.add(`${bed}_initial_layer`); out.add(bed); }
    }
  }
  return out;
}

export type HiddenReason = 'other plate' | 'empty' | 'not applicable';

export function hiddenReason(k: string, s: State, type?: PresetType, slot?: number): HiddenReason | undefined {
  const [key, idx] = splitKey(k), def = OPTIONS[key];
  if (!def) return undefined;
  type ??= (Object.keys(TAB) as PresetType[]).find((t) => TABS[TAB[t]]?.pages.some((p) =>
    p.groups.some((g) => g.options.some((k) => splitKey(k)[0] === key)))) ?? 'filament';
  if (type === 'filament' && bedKeys.has(key) && !commonKeys(type, s).has(key)) return 'other plate';
  slot ??= type === 'filament' && s.dialog?.type === 'filament' ? s.dialog.slot : 0;
  const value = effectiveConfig(s, type, slot)[key];
  const element = (v: unknown) => Array.isArray(v) && /^(?:floats|ints|percents)/.test(def.type)
    ? v[Math.min(idx, v.length - 1)] : v;
  if (emptyRanges.has(key)) {
    const v = element(value);
    if (empty(v) || key.startsWith('nozzle_temperature_range_') && Number(scalar(v)) === 0) return 'empty';
  } else if (/^(?:float|int|percent)/.test(def.type) && empty(element(value))) return 'empty';
  const machine = effectiveConfig(s, 'machine');
  const off = (name: string) => {
    const v = scalar(machine[name]);
    return v === false || v === 0 || v === '0' || v === 'false';
  };
  if (type === 'filament' && key === 'additional_cooling_fan_speed' && off('auxiliary_fan')) return 'not applicable';
  if (type === 'filament' && /^(?:activate_air_filtration|during_print_exhaust_fan_speed|complete_print_exhaust_fan_speed)/.test(key)
    && off('support_air_filtration')) return 'not applicable';
  if (type === 'filament' && key === 'activate_chamber_temp_control' && off('support_chamber_temp_control')) return 'not applicable';
  if (type === 'filament' && /^chamber_(?:minimal_)?temperature$/.test(key) && off('support_chamber_temp_control')
    && !/chamber_(?:minimal_)?temperature/.test(['machine_start_gcode', 'machine_end_gcode', 'change_filament_gcode'].map((k) => String(scalar(machine[k]) ?? '')).join('\n'))) return 'not applicable';
  return undefined;
}

export function commonMode(type: PresetType, key: string, s: State, native: OptionMode): OptionMode {
  return commonKeys(type, s).has(splitKey(key)[0]) ? 'simple' : native === 'simple' ? 'advanced' : native;
}

export function groupCounts(type: PresetType, page: TabPage | string, group: TabGroup | string, s: State,
  slot = type === 'filament' && s.dialog?.type === 'filament' ? s.dialog.slot : 0): { shown: number; more: number } {
  const desc = TABS[TAB[type]];
  const p = typeof page === 'string' ? desc?.pages.find((p) => p.title === page) : page;
  const g = typeof group === 'string' ? p?.groups.find((g) => g.title === group) : group;
  if (!desc || !g) return { shown: 0, more: 0 };
  const env = tabEnvironment(s, type, slot, effectiveMode(s));
  if (p?.condition && condition(p.condition, env) === false || g.condition && condition(g.condition, env) === false)
    return { shown: 0, more: 0 };
  const states = optionState(desc, env), seen = new Set<string>();
  let shown = 0, more = 0;
  for (const line of g.lines) for (const k of line.options) {
    const key = splitKey(k)[0], mode = lineMode(line, k);
    if (!OPTIONS[key] || !mode || isHidden(key) || seen.has(key)) continue;
    seen.add(key);
    const focused = s.settingsFocus?.type === type && s.settingsFocus.key === key;
    if (!focused && line.options.some((o) => states[splitKey(o)[0]]?.visible === false)) continue;
    if (settingVisible(type, k, s, mode, false, slot)) shown++;
    else if (modeVisible(commonMode(type, key, s, mode), 'advanced') && settingVisible(type, k, { ...s, uiMode: 'advanced' }, mode, true, slot)) more++;
  }
  return { shown, more };
}
