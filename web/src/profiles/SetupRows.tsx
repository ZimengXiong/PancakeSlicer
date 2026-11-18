import { setState, toast, useStore } from '../state/store';
import { undo } from '../state/history';
import { flushModified, flushUnavailable } from '../flush-state';
import { plateSetup, setBuildPlate } from './plate-setup';
import { Dropdown } from '../components/Dropdown';
import { Glyph } from '../components/Glyph';
import { MenuButton, type MenuEntry } from '../components/Menu';
import './profiles.css';
import { NozzleSize } from './NozzleSize';
import { slotFilaments, slotMixes } from './slots';
import { slotExtruder } from '../printers/feeds';

export function NozzleField() {
  const printer = useStore((s) => s.printer);
  return printer ? <NozzleSize printerId={printer} grid/> : null;
}

const PLATE_SHORT: Record<string, string> = { 'Smooth Cool Plate': 'Cool Plate', 'Engineering Plate': 'Engineering', 'Smooth High Temp Plate': 'High Temp',
  'Smooth PEI Plate': 'Smooth PEI', 'Textured PEI Plate': 'Textured PEI', 'Textured Cool Plate': 'Textured Cool', 'Cool Plate (SuperTack)': 'SuperTack' };

export function PlateTypeField({ bare = false }: { bare?: boolean }) {
  const setup = useStore((s) => plateSetup(s));
  if (!setup?.plates.length) return null;
  const options = setup.plates.map((p) => ({ value: p.value, label: p.label, short: PLATE_SHORT[p.label] }));
  const field = <Dropdown className="combo rail-plate-combo" aria-label="Build plate" value={setup.plate}
    onValueChange={(value) => void setBuildPlate(value)} options={options} />;
  return bare ? field : <div className="rail-row rail-plate">{field}</div>;}

export function BuildPlateField({ printerId, disabled = false }: { printerId: string; disabled?: boolean }) {
  const setup = useStore((s) => plateSetup(s, printerId));
  if (!setup?.plates.length) return null;
  return <div className="nozzle-row"><span className="opt-label">Build plate</span><span className="opt-field">
    <Dropdown className="combo" aria-label="Build plate" value={setup.plate} disabled={disabled}
      onValueChange={(value) => void setBuildPlate(value, printerId)} options={setup.plates.map((p) => ({ value: p.value, label: p.label }))} /></span></div>;
}

export function addFilament() {
  void import('../plater/mixed').then((m) => m.addPhysical()).catch((error) => toast('add_physical_failed', { detail: String(error) }));
}

export function removeSlot(i: number) {
  void import('../plater/mixed').then(({ removeSlotAt }) => {
    const done = removeSlotAt(i + 1);
    if (!done) return;
    const notes = [done.moved ? `${done.moved} object${done.moved === 1 ? '' : 's'} moved to slot ${done.to}` : '', ...done.merged.map((d) => `mix ${d.mix} merged into slot ${d.into}`)].filter(Boolean);
    toast('filament_removed', { slot: i + 1, notes: notes.join('; ') }, { action: { label: 'Undo', run: undo } });
  });
}

export function FilamentMenu({ extra = [] }: { extra?: MenuEntry[] }) {
  const slots = useStore((s) => slotFilaments(s).length);
  const physical = slots - Object.keys(useStore((s) => slotMixes(s))).length;
  const flushChanged = useStore((st) => flushModified(st));
  const flushReason = useStore(flushUnavailable);
  const mixable = useStore((st) => {
    const printer = st.records.printers.find((p) => p.id === st.printer), count = printer?.nozzles.length ?? 1, n = slotFilaments(st).length - Object.keys(slotMixes(st)).length;
    const sides = Array.from({ length: n }, (_, i) => slotExtruder(printer, count, i));
    return sides.some((e, i) => e !== undefined && sides.indexOf(e) !== i);
  });
  const items: MenuEntry[] = [...extra,
    ...(physical > 1 ? [flushReason ? { label: 'Flushing volumes…', disabled: true, reason: flushReason }
      : { label: flushChanged ? 'Flushing volumes (edited)…' : 'Flushing volumes…', action: () => setState({ flushDialog: true }) }] : []),
    ...(physical > 1 ? [mixable ? { label: 'Mixed filament…', action: () => setState({ appDialog: 'mixed' }) }
      : { label: 'You\'ll need two slots on one nozzle to mix filaments!', disabled: true }] : []),
  ];
  return items.length ? <MenuButton className="icon-btn rail-act" label="Slot tools" items={items}><Glyph name="more"/></MenuButton> : null;
}
