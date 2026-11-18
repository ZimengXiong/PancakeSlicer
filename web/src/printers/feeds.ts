import feeds from '../../../server/printer_feeds.json';
import type { PrinterRecord, SlotRecord, SwapMode } from '../profiles/records';
import { randomId } from '../random-id';

interface SlotSide { slots: readonly { tray?: string }[]; units?: FeedUnit[]; swap?: SwapMode }

export interface Feed { unit: string; slots: number; maxUnits: number; external: 'exclusive' | 'perNozzle'; nozzles?: string[] }
export interface FeedUnit { nozzle: number }
export interface SlotPosition { tray: string; label: string; group?: string }

export const feedOf = (model: string | undefined): Feed | undefined => model ? (feeds.models as Record<string, Feed>)[model] : undefined;

export function unitsOf(feed: Feed, setup: Pick<SlotSide, 'units' | 'slots'>): FeedUnit[] {
  if (setup.units) return setup.units;
  const externals = feed.external === 'perNozzle' ? feed.nozzles?.length ?? 1 : 1;
  const extra = feed.external === 'perNozzle' ? setup.slots.length - externals : setup.slots.length > 1 ? setup.slots.length : 0;
  return Array.from({ length: Math.min(feed.maxUnits, Math.ceil(Math.max(0, extra) / feed.slots)) }, () => ({ nozzle: 0 }));
}

export function slotPositions(feed: Feed, units: FeedUnit[]): SlotPosition[] {
  let n = 0;
  const amsSlots = (unit: number) => Array.from({ length: feed.slots }, (_, s): SlotPosition => ({ tray: `ams${unit}.${s}`, label: `Slot ${++n}` }));
  if (feed.external === 'exclusive') return units.length ? units.flatMap((_, u) => amsSlots(u)) : [{ tray: 'ext', label: 'External' }];
  const names = feed.nozzles ?? [''], fed = (nozzle: number) => units.some((unit) => unit.nozzle === nozzle);
  const order = names.map((_, nozzle) => nozzle).sort((a, b) => Number(fed(b)) - Number(fed(a)) || a - b);
  return order.flatMap((nozzle) => [
    ...units.flatMap((unit, u) => unit.nozzle === nozzle ? amsSlots(u) : []),
    { tray: nozzle === 0 ? 'ext.L' : 'ext.R', label: 'External' },
  ].map((p) => ({ ...p, group: `${names[nozzle]} nozzle` })));
}

export function nextTray(printer: Pick<PrinterRecord, 'name' | 'model' | 'nozzles' | 'swap' | 'units'>, slots: readonly { tray?: string }[]): string | undefined {
  const feed = feedOf(printer.model);
  if (!feed || printer.swap === 'toolheads') return undefined;
  const positions = slotPositions(feed, unitsOf(feed, { units: printer.units, slots })), held = new Set(slots.map((slot) => slot.tray));
  const free = positions.find((p) => !held.has(p.tray));
  if (!free) throw new Error(`${printer.name} doesn't have any free trays for another slot. Attach another ${feed.unit} unit on Device to add a slot!`);
  return free.tray;
}

export function layoutSetup(printer: PrinterRecord, units?: FeedUnit[], assign = false): PrinterRecord {
  const feed = feedOf(printer.model);
  if (!feed) return printer;
  const attached = units ?? unitsOf(feed, printer), positions = slotPositions(feed, attached), at = new Set(positions.map((p) => p.tray));
  const slots: SlotRecord[] = printer.slots.flatMap((slot) => {
    if (!slot.tray || at.has(slot.tray)) return [slot];
    const { tray: _gone, ...rest } = slot;
    return slot.filament ? [rest] : [];
  });
  const held = new Set(slots.flatMap((slot) => slot.tray ? [slot.tray] : []));
  const free = positions.filter((p) => !held.has(p.tray));
  if (assign) for (const [i, slot] of slots.entries()) if (!slot.tray && free.length) slots[i] = { ...slot, tray: free.shift()!.tray };
  const ids = new Set(slots.map((slot) => slot.id));
  for (const p of free) slots.push({ id: ids.has(`slot:${printer.id}:${p.tray}`) ? `slot:${randomId()}` : `slot:${printer.id}:${p.tray}`, filament: '', tray: p.tray });
  const same = slots.length === printer.slots.length && slots.every((s, i) => s === printer.slots[i])
    && JSON.stringify(attached) === JSON.stringify(printer.units);
  return same ? printer : { ...printer, units: attached, slots };
}

export function slotLabels(setup: SlotSide | undefined, model: string | undefined, count: number): SlotPosition[] {
  const feed = feedOf(model), positions = feed && setup ? slotPositions(feed, unitsOf(feed, setup)) : [];
  return Array.from({ length: count }, (_, i) => {
    const tray = setup?.slots[i]?.tray ?? '';
    return positions.find((p) => tray && p.tray === tray) ?? { tray, label: feed && i < (setup?.slots.length ?? 0) ? 'No tray' : `Slot ${i + 1}` };
  });
}

export function slotExtruder(setup: SlotSide | undefined, nozzles: number, i: number): number | undefined {
  if (nozzles <= 1) return 0;
  if (setup?.swap === 'toolheads') return i < nozzles ? i : undefined;
  const tray = setup?.slots[i]?.tray ?? '';
  if (tray === 'ext' || tray === 'ext.L') return 0;
  if (tray === 'ext.R') return 1;
  const unit = /^ams(\d+)\./.exec(tray);
  return unit ? setup?.units?.[Number(unit[1])]?.nozzle : undefined;
}
