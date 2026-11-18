import type { ConfigValue } from '../api/types';
import { getState, subscribeStore, toast } from '../state/store';
import { plateSlots } from '../printers/slot-check';
import { selectionValues, profileInfo } from '../profiles/view';
import { slotFilaments } from '../profiles/slots';

let stop: (() => void) | null = null;
const text = (v: ConfigValue | undefined) => String(Array.isArray(v) ? v[0] : v ?? '');

export function noteSpeedCap(key: string, typed: ConfigValue) {
  stop?.();
  const want = text(typed);
  const check = () => {
    const s = getState(), asked = s.resolved.answeredFor?.project.process?.[key];
    if (s.resolved.state !== 'ready' || text(asked) !== want) return;
    stop?.(); stop = null;
    const used = plateSlots(s), nozzle = selectionValues(s, 'process').nozzles?.find((n) => n.slots.some((slot) => used.includes(slot + 1)));
    const d = nozzle?.speeds[key], requested = Number(want);
    if (!d || !(requested > d.speed + 1e-6)) return;
    const fastest = nozzle!.slots.filter((slot) => used.includes(slot + 1))
      .map((slot) => ({ slot, flow: Number(text(selectionValues(s, 'filament', slot).values.filament_max_volumetric_speed)) }))
      .sort((a, b) => b.flow - a.flow)[0];
    const by = d.limit === 'flow cap' ? `${fastest ? profileInfo(s, 'filament', slotFilaments(s)[fastest.slot]).name : 'the filament'}'s flow`
      : d.limit === 'printer max' ? 'the printer\'s top speed' : d.limit === 'outer-wall ceiling' ? 'the outer-wall ceiling' : d.limit;
    toast('tops_out_mm_kept', { speed: d.speed, by, want });
  };
  stop = subscribeStore(check);
  check();
}
