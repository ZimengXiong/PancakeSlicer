import type { Config, ConfigValue, PresetType } from '../api/types';
import { sameValue } from './value-equality';
export { sameValue } from './value-equality';
import { isHidden, profileHolds } from '../options';
import { effectiveConfig, PRESET_META, presetEdits, type State } from './store';
import { valueBelow, selectionValues } from '../profiles/view';

export type ChangeScope = 'project' | 'object' | 'part' | 'range';
export interface ChangeTarget { object?: number; volume?: number; range?: number }
export interface ChangeOwner extends ChangeTarget { name: string; plate?: number }
export interface SettingChange {
  type: PresetType;
  slot: number;
  key: string;
  was: ConfigValue | undefined;
  now: ConfigValue;
  scope: ChangeScope;
  owner: ChangeOwner;
}


export function presetValue(s: State, type: PresetType, key: string, slot = 0): ConfigValue | undefined {
  const layers = selectionValues(s, type, slot).layers[key];
  return layers ? valueBelow(layers, 'project') : undefined;
}

export function settingValue(s: State, type: PresetType, key: string, slot = 0, target: ChangeTarget = {}): ConfigValue | undefined {
  let value: ConfigValue | undefined = effectiveConfig(s, type, slot)[key];
  if (type !== 'process') return value;
  const object = s.objects.find((o) => o.id === target.object);
  if (!object) return value;
  value = object.settings[key] ?? value;
  const volume = object.volumes?.find((v) => v.id === target.volume);
  value = volume?.settings[key] ?? value;
  const range = object.ranges?.find((r) => r.id === target.range);
  return range ? key === 'layer_height' ? range.layer_height : range.settings[key] ?? value : value;
}

const plateOwns = (edits: Config, plate: { settings: Config } | undefined, key: string) => edits[key] === undefined && plate?.settings[key] !== undefined;

export function authoredChanges(s: State, type: PresetType, slot = 0): Config {
  const edits = presetEdits(s, type, slot);
  const project = (type === 'filament' ? s.projectPresets?.filaments[slot] : s.projectPresets?.[type]) ?? {};
  const plate = type === 'process' ? s.projectPlates.find((p) => p.index === s.plateIndex) : undefined;
  const out: Config = {};
  for (const key of new Set([...Object.keys(project), ...Object.keys(edits)])) {
    const value = edits[key] ?? project[key];
    if (value === undefined || plateOwns(edits, plate, key) || isHidden(key) || PRESET_META.test(key) || !profileHolds(type, key)) continue;
    out[key] = value;
  }
  return out;
}

export function unsavedRule(edited: Record<string, unknown>, values: Config, below: Config, sources?: Record<string, string | undefined>): (key: string) => boolean {
  return (key) => key in edited && (!sameValue(key, values[key], below[key]) || sources?.[key] === 'set');
}

export function selectChanges(s: State, type: PresetType, slot = 0): SettingChange[] {
  const rows: SettingChange[] = [];
  const add = (key: string, was: ConfigValue | undefined, now: ConfigValue, scope: ChangeScope, owner: ChangeOwner) => {
    if (!isHidden(key) && !PRESET_META.test(key) && !sameValue(key, was, now)) rows.push({ type, slot, key, was, now, scope, owner });
  };
  const edits = presetEdits(s, type, slot);
  const project = type === 'filament' ? s.projectPresets?.filaments[slot] : s.projectPresets?.[type];
  const plate = type === 'process' ? s.projectPlates.find((p) => p.index === s.plateIndex) : undefined;
  const global = effectiveConfig(s, type, slot);
  const globalValue = (key: string): ConfigValue | undefined => global[key];
  for (const key of new Set([...Object.keys(project ?? {}), ...Object.keys(plate?.settings ?? {}), ...Object.keys(edits)])) {
    const now = global[key];
    if (now === undefined || !profileHolds(type, key)) continue;
    const owner: ChangeOwner = plateOwns(edits, plate, key) && plate ? { name: plate.name || `Plate ${plate.index}`, plate: plate.index } : { name: 'Project' };
    add(key, presetValue(s, type, key, slot), now, 'project', owner);
  }
  if (type !== 'process') return rows;
  for (const object of s.objects) {
    const objectValue = (key: string) => object.settings[key] ?? globalValue(key);
    for (const [key, now] of Object.entries(object.settings))
      add(key, globalValue(key), now, 'object', { name: object.name, object: object.id });
    for (const volume of object.volumes ?? []) for (const [key, now] of Object.entries(volume.settings))
      add(key, objectValue(key), now, 'part', { name: `${object.name} › ${volume.name}`, object: object.id, volume: volume.id });
    for (const range of object.ranges ?? []) {
      const owner = { name: `${object.name} › Range ${range.z[0].toFixed(2)}-${range.z[1].toFixed(2)} mm`, object: object.id, range: range.id };
      add('layer_height', objectValue('layer_height'), range.layer_height, 'range', owner);
      const parts = object.volumes?.filter((v) => v.type === 'part') ?? [];
      for (const [key, now] of Object.entries(range.settings)) {
        if (key === 'layer_height') continue;
        if (!parts.length) add(key, objectValue(key), now, 'range', owner);
        else for (const part of parts) add(key, part.settings[key] ?? objectValue(key), now, 'range', {
          ...owner, name: `${owner.name} › ${part.name}`, volume: part.id,
        });
      }
    }
  }
  return rows;
}
