import type { Config } from '../api/types';
import { getState, type State } from '../state/store';
import { editSetup } from '../printers/device-slots';
import { paletteName } from '../components/filament-palette';
import { DEFAULT_FILAMENT_COLOUR as DEFAULT_COLOUR } from './metadata';
import { isExternalTray } from '../multinozzle/ams';
import { mutation, responseError } from '../api/access';
import { mixAt, slotAt, slotMixes } from './slots';
import { blend } from '../plater/mixColour';

const hex = (value: unknown): string | undefined => {
  const v = String(Array.isArray(value) ? value[0] ?? '' : value ?? '');
  return /^#[\da-f]{6}/i.test(v) ? v.slice(0, 7).toUpperCase() : undefined;
};
export const configColour = (config: Config) => hex(config.filament_colour) ?? hex(config.default_filament_colour);

export const EMPTY_SLOT_COLOUR = '#7A7A7A';
export function reportedTray(s: State, slot: number, printer = s.printer): { colour: string } | { empty: true } | undefined {
  const tray = slotAt(s, printer, slot)?.tray, status = s.printerConnections.status[printer];
  if (!tray?.startsWith('ams') || !status?.online || !status.trays) return undefined;
  const t = status.trays.find((x) => !isExternalTray(x.id) && `ams${x.ams}.${x.slot}` === tray), colour = t?.type ? hex(t.color) : undefined;
  return colour ? { colour } : { empty: true };
}
export function slotColour(s: State, slot: number, printer = s.printer): string {
  const mix = mixAt(s, printer, slot);
  if (mix) return blend(mix.components.map((c) => c >= 1 && slotAt(s, printer, c - 1) ? slotColour(s, c - 1, printer) : '#808080'), mix.ratios);
  const reported = reportedTray(s, slot, printer);
  if (reported) return 'colour' in reported ? reported.colour : EMPTY_SLOT_COLOUR;
  return hex(slotAt(s, printer, slot)?.colour) ?? DEFAULT_COLOUR;
}

export function setSlotColour(slot: number, colour: string, printer = getState().printer): Promise<void> {
  const value = hex(colour), s = getState();
  if (!value) throw new Error('Pick a color first.');
  if (printer === s.printer && slotMixes(s)[slot]) throw new Error('A mixed slot gets its colour from the filaments it blends together.');
  const id = slotAt(s, printer, slot)?.id;
  if (!id) throw new Error('That slot is no longer on this printer.');
  return editSetup(printer, (setup) => ({ ...setup, slots: setup.slots.map((x) => (x.id === id ? { ...x, colour: value } : x)) }));
}

export async function applySlotColour(slot: number, colour: string, printer = getState().printer): Promise<string | undefined> {
  const s = getState(), value = hex(colour), target = slotAt(s, printer, slot), connection = s.printerConnections.connections.find((c) => c.printer === printer);
  if (!value) throw new Error('Pick a color first.');
  if (connection?.kind === 'bambu-lan' && reportedTray(s, slot, printer)) {
    const r = await mutation(`/api/printer-connections/${encodeURIComponent(printer)}/tray`, { method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ slot: target?.tray, colour: value, filament: target?.filament ?? '' }) });
    if (!r.ok) throw await responseError(r);
    return undefined;
  }
  await setSlotColour(slot, value, printer);
  const online = connection && s.printerConnections.status[printer]?.online;
  return online && connection.kind !== 'bambu-lan' ? 'Saved here; this printer doesn\'t store slot colours' : undefined;
}

const COLOUR_NAMES: [number, string][] = [[15, 'Red'], [40, 'Orange'], [65, 'Yellow'], [165, 'Green'], [195, 'Cyan'], [255, 'Blue'], [290, 'Purple'], [335, 'Pink'], [360, 'Red']];
export function colourName(colour: string): string {
  const v = hex(colour) ?? DEFAULT_COLOUR, preset = paletteName(v);
  if (preset) return preset;
  const [r, g, b] = [1, 3, 5].map((i) => parseInt(v.slice(i, i + 2), 16) / 255);
  const max = Math.max(r, g, b), min = Math.min(r, g, b), l = (max + min) / 2, d = max - min;
  const sat = d === 0 ? 0 : d / (1 - Math.abs(2 * l - 1));
  if (l < 0.12) return 'Black';
  if (l > 0.92) return 'White';
  if (sat < 0.15) return l > 0.6 ? 'Silver' : 'Gray';
  const h = d === 0 ? 0 : max === r ? 60 * (((g - b) / d) % 6) : max === g ? 60 * ((b - r) / d + 2) : 60 * ((r - g) / d + 4);
  const hue = (h + 360) % 360, name = COLOUR_NAMES.find(([limit]) => hue < limit)![1];
  return (name === 'Orange' || name === 'Red') && l < 0.35 ? 'Brown' : name;
}
