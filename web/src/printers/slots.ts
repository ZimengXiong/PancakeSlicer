import { useEffect, useState } from 'react';
import { mutation, responseError } from '../api/access';
import { useStore, type State } from '../state/store';
import { isExternalTray } from '../multinozzle/ams';
import { colourName } from '../profiles/filament-colour';
import { useAskedValues, variantOf } from '../profiles/view';
import { loadedLine, tuningFor } from '../catalog/read';
import { valueAt } from '../multinozzle/config';
import type { Tray } from './connections';

export interface SlotReport {
  slot: string; label: string;
  tray: { type: string; colour: string; name: string; filament_id: string };
  remaining: number | null; spool: string | null;
  earlier?: string;
}
export interface PrinterSlots { online: boolean; updated: string; slots: SlotReport[]; error?: string }

export const slotOf = (t: Tray) => isExternalTray(t.id) ? t.extruder === undefined ? 'ext' : t.extruder === 0 ? 'ext.L' : 'ext.R' : `ams${t.ams}.${t.slot}`;

const reportedProduct = (name: string) => name ? (/^bambu\b/i.test(name) ? name : `Bambu ${name}`) : '';

export function reportText(r: Pick<SlotReport, 'tray' | 'remaining'> | undefined): string {
  if (!r || !r.tray.type) return 'Printer: empty';
  const name = reportedProduct(r.tray.name) || `unknown filament (${r.tray.type})`;
  const colour = /^#[0-9a-f]{6}/i.test(r.tray.colour) ? colourName(r.tray.colour.slice(0, 7)) : '';
  return `Printer: ${[name, colour, r.remaining != null && r.remaining >= 0 ? `${Math.min(100, r.remaining)}%` : ''].filter(Boolean).join(', ')}`;
}

export function trayLibraryFilament(s: State, printer: string, r: SlotReport | undefined): string | undefined {
  if (!r?.tray.type) return undefined;
  if (r.earlier) return r.earlier;
  const live = s.records.filaments.filter((f) => !f.archived && f.product), id = r.tray.filament_id, variant = variantOf(s, printer).id;
  const byId = id ? live.find((f) => { const line = loadedLine('filaments', f.product); return !!line && tuningFor(line, variant)?.source.filament_id === id; }) : undefined;
  const product = reportedProduct(r.tray.name);
  return (byId ?? (product ? live.find((f) => f.product === product) : undefined))?.id;
}

export const mismatch = (trayType: string, filamentType: string) => {
  const head = (t: string) => t.trim().toUpperCase().split(/[\s-]+/)[0];
  return !!trayType && !!filamentType && head(trayType) !== head(filamentType);
};

export function useFilamentType(printer: string, filament: string, extruder = 0): string | undefined {
  const v = useAskedValues(printer && filament ? { type: 'filament', id: filament, printer, extruder } : null);
  return v?.state === 'ready' ? valueAt(v.values.filament_type, 0) : undefined;
}

export function materialNote(trayType: string, name: string, filamentType: string | undefined): string | null {
  if (!trayType || filamentType === undefined) return null;
  if (!filamentType) return `${name} has no material type, so it can't be checked against the printer's ${trayType}.`;
  return mismatch(trayType, filamentType) ? `Hmm, your printer says that's ${trayType}, while ${name} is ${filamentType}.` : null;
}

const path = (printer: string, slot = '') => `/api/printer-slots/${encodeURIComponent(printer)}${slot && `/${encodeURIComponent(slot)}`}`;

export async function readPrinterSlots(printer: string): Promise<PrinterSlots> {
  const r = await fetch(path(printer));
  if (!r.ok) throw await responseError(r);
  return await r.json() as PrinterSlots;
}

export async function rememberSpool(printer: string, slot: string, filament: string | null): Promise<PrinterSlots> {
  const r = await mutation(path(printer, slot), filament
    ? { method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ filament }) }
    : { method: 'DELETE' });
  if (!r.ok) throw await responseError(r);
  return await r.json() as PrinterSlots;
}

export function usePrinterSlots(printer: string | undefined): { slots?: PrinterSlots; error?: string; set(next: PrinterSlots): void } {
  const trays = useStore((s) => (printer ? JSON.stringify(s.printerConnections.status[printer]?.trays ?? null) : ''));
  const library = useStore((s) => s.profileLibraryRevision);
  const [state, setState] = useState<{ key: string; printer?: string; slots?: PrinterSlots; error?: string }>({ key: '' });
  const key = `${printer}\n${trays}\n${library}`;
  useEffect(() => {
    if (!printer) return;
    let live = true;
    readPrinterSlots(printer).then((slots) => { if (live) setState({ key, printer, slots }); }, (e: unknown) => live && setState({ key, printer, error: e instanceof Error ? e.message : String(e) }));
    return () => { live = false; };
  }, [key]); // eslint-disable-line react-hooks/exhaustive-deps
  const current = state.key === key, same = state.printer === printer;
  return { slots: current || same ? state.slots : undefined, error: current ? state.error : undefined, set: (slots) => setState({ key, printer, slots }) };
}
