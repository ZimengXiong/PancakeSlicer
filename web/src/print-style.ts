import type { Config, ConfigValue } from './api/types';
import type { IconId } from './components/icons';
import { OPTIONS } from './options';
import { STYLE_NAMES, styleOf, type StyleId } from './style-names';

export { styleOf, type StyleId };
import { undoable } from './state/history';
import { effectiveConfig, getState, removeObjectOptions, resetOption, resolvePreset, setObjectOption, setOption } from './state/store';
import { sameValue } from './state/value-equality';

export interface Style {
  id: StyleId;
  name: string;
  label?: string;
  tip: string;
  icon: IconId;
  keys: Config;
  rotary?: boolean;
}

export const STYLES: Style[] = [
  { id: 'standard', name: STYLE_NAMES.standard, label: 'Flat', tip: 'Standard horizontal layers—fast and reliable.', icon: 'standard', keys: { nonplanar_mode: 'off', zaa_enabled: '0' } },
  { id: 'smooth', name: STYLE_NAMES.smooth, label: 'Curved tops', tip: 'Top layers curve smoothly along gentle slopes wherever the print head fits.', icon: 'smooth', keys: { nonplanar_mode: 'nonplanar_top' } },
  { id: 'morph', name: STYLE_NAMES.morph, label: 'Morph', tip: 'Layers smoothly transition from flat at the bed to curved at the top.', icon: 'smooth', keys: { nonplanar_mode: 'smooth_morph' } },
  { id: 'zaa', name: STYLE_NAMES.zaa, tip: 'Flat layers with top toolpaths contoured to follow slopes.', icon: 'zaa', keys: { nonplanar_mode: 'off', zaa_enabled: '1' } },
  { id: 'multiplanar', name: STYLE_NAMES.multiplanar, tip: 'Tilts sections so overhangs print cleanly with less support.', icon: 'chunks', keys: { nonplanar_mode: 'multiplanar' }, rotary: true },
  { id: 'field', name: STYLE_NAMES.field, label: 'Rotary layers', tip: 'Full curved layers while the rotary table tilts to keep the nozzle level.', icon: 'rotary', keys: { nonplanar_mode: 'field' }, rotary: true },
];

export const styleById = (id: StyleId) => STYLES.find((s) => s.id === id)!;

export const isRotary = (machine: Config) => String(machine.nonplanar_kinematics ?? 'none') !== 'none';

const asText = (v: ConfigValue | undefined) => (Array.isArray(v) ? String(v[0] ?? '') : String(v ?? ''));

export function modeLabel(c: Config) {
  const mode = asText(c.nonplanar_mode) || 'off';
  const def = OPTIONS.nonplanar_mode;
  return def?.enum_labels?.[def.enum_values?.indexOf(mode) ?? -1] ?? mode;
}

const same = (key: string, a: ConfigValue | undefined, b: ConfigValue | undefined) => sameValue(key, a ?? '', b ?? '');

export function applyStyle(id: StyleId, objectId?: number) {
  undoable(`Change print style to "${styleById(id).name}"`, () => writeStyle(id, objectId));
}

function writeStyle(id: StyleId, objectId?: number) {
  const s = getState();
  const keys = styleById(id).keys;
  if (objectId === undefined) {
    const system = resolvePreset(s, 'process', s.process).config;
    for (const [k, v] of Object.entries(keys)) {
      if (same(k, v, system[k])) resetOption('process', k);
      else setOption('process', k, v);
    }
    return;
  }
  const global = effectiveConfig(s, 'process');
  const drop = Object.keys(keys).filter((k) => same(k, keys[k], global[k]));
  for (const [k, v] of Object.entries(keys)) if (!drop.includes(k)) setObjectOption(objectId, k, v);
  if (drop.length) removeObjectOptions(objectId, drop);
}

export const processShortName = (name: string) => name.replace(/\s*@.*$/, '');
