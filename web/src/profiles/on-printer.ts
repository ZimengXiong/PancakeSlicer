import { useMemo } from 'react';
import { getState, useStore, type State } from '../state/store';
import { slotLabels } from '../printers/feeds';
import { printerModel } from '../printers/capabilities';
import { setupOf } from '../printers/device-slots';
import { findProfile } from './identity';

export interface Placement { printer: string; slots: string[] }

export function useOnPrinter(enabled: boolean): ReadonlyMap<string, Placement[]> {
  const key = useStore((s) => enabled ? s.records.printers : null);
  return useMemo(() => enabled ? placements(getState()) : new Map(), [enabled, key]); // eslint-disable-line react-hooks/exhaustive-deps
}

export function placements(s: State): Map<string, Placement[]> {
  const out = new Map<string, Placement[]>();
  for (const printer of s.records.printers.map((p) => p.id)) {
    const machine = findProfile(s, 'machine', printer);
    if (!machine || machine.archived) continue;
    const setup = setupOf(s, printer);
    if (!setup) continue;
    const labels = slotLabels(setup, printerModel(s, printer), setup.slots.length);
    setup.slots.forEach((slot, i) => {
      if (!slot.filament) return;
      const list = out.get(slot.filament) ?? [];
      let row = list.find((p) => p.printer === machine.name);
      if (!row) list.push(row = { printer: machine.name, slots: [] });
      row.slots.push(labels[i].group ? `${labels[i].group} ${i + 1}` : labels[i].label);
      out.set(slot.filament, list);
    });
  }
  return out;
}
export const filamentPlacements = (s: State, key: string) => placements(s).get(key) ?? [];

export function placementText(list: readonly Placement[] | undefined): string {
  return (list ?? []).map((p) => {
    const numbers = p.slots.map((l) => l.match(/^Slot (\d+)$/)?.[1]), all = numbers.every(Boolean);
    return `${p.printer}: ${all ? `${numbers.length > 1 ? 'Slots' : 'Slot'} ${numbers.join(', ')}` : p.slots.join(', ')}`;
  }).join(' | ');
}
