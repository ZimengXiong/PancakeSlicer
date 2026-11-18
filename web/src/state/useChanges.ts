import { useMemo } from 'react';
import type { PresetType } from '../api/types';
import { selectChanges } from './changes';
import { effectiveConfig, presetEdits, resolvePreset, useStore, type State } from './store';
import { slotFilaments } from '../profiles/slots';

export function useChanges(type: PresetType, slot = 0) {
  const select = useMemo(() => {
    let last: State | undefined, snapshot: State, previous: unknown[] = [];
    return (s: State) => {
      if (s === last) return snapshot;
      const name = type === 'machine' ? s.printer : type === 'process' ? s.process : slotFilaments(s)[slot];
      const project = type === 'filament' ? s.projectPresets?.filaments[slot] : s.projectPresets?.[type];
      const plate = type === 'process' ? s.projectPlates.find((p) => p.index === s.plateIndex) : undefined;
      const inputs: unknown[] = [effectiveConfig(s, type, slot), resolvePreset(s, type, name ?? '').config,
        presetEdits(s, type, slot), project, plate?.settings, plate?.index, plate?.name];
      if (type === 'process') for (const o of s.objects) {
        inputs.push(o.id, o.name, o.settings);
        for (const v of o.volumes ?? []) inputs.push('volume', v.id, v.name, v.type, v.settings);
        for (const r of o.ranges ?? []) inputs.push('range', r.id, r.z[0], r.z[1], r.layer_height, r.settings);
      }
      last = s;
      if (!snapshot || inputs.length !== previous.length || inputs.some((v, i) => v !== previous[i])) {
        previous = inputs;
        snapshot = s;
      }
      return snapshot;
    };
  }, [type, slot]);
  return selectChanges(useStore(select), type, slot);
}
