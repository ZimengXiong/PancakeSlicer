import { noticeText } from '../notice-codes';
import { getState, toast } from '../state/store';
import { availableName } from './catalog';
import { commitRecords } from './transactions';
import type { FilamentRecord, LibraryRecords, PrinterRecord } from './records';
import { printerVendors, type CatalogModel, type CatalogVendor } from './catalog-reads';
import { catalogLine, defaultVariant, firmwareName, firmwareOf, listRead, useCatalogRead } from '../catalog/read';
import type { CatalogEntry } from '../catalog/types';
import { feedOf, layoutSetup } from '../printers/feeds';
import { randomId } from '../random-id';

export type CatalogRead = { vendors: CatalogVendor[] } | { products: CatalogEntry[] };
export function useCatalog(type: 'machine' | 'filament' | 'process', adding: boolean): { data?: CatalogRead; error?: string } {
  const read = useCatalogRead(listRead(type === 'machine' ? 'printers' : 'filaments'));
  if (!adding || type === 'process') return {};
  if (!read.value) return read.error ? { error: read.error } : {};
  return type === 'machine' ? { data: { vendors: printerVendors(read.value) } } : { data: { products: read.value.entries } };
}

const plain = (name: string) => name.replace(/\s+@.*$/, '').trim();

export type PrinterIntent = 'add' | 'another';
export interface PrinterChoice { vendor: string; model: CatalogModel; nozzle: string; firmware?: string; intent?: PrinterIntent }
export interface ProductChoice { product: CatalogEntry; label?: string }
export interface AddResult { keys: string[]; existing: string[]; skipped: string[]; chosen: string[]; restored?: string[] }

function namer(records: { name: string; archived?: unknown }[]) {
  const taken = new Set(records.filter((r) => !r.archived).map((r) => r.name));
  return (wanted: string) => { const name = availableName(taken, wanted); taken.add(name); return name; };
}
type Plan<R> = { records: R[]; existing: string[]; skipped: string[]; chosen: string[]; wanted: string[]; restored?: R[] };
async function add<R extends PrinterRecord | FilamentRecord>(kind: 'printer' | 'filament', plan: (doc: LibraryRecords) => Plan<R>): Promise<AddResult> {
  const result = plan(getState().records), restored = result.restored ?? [];
  if (result.records.length || restored.length)
    await commitRecords([...result.records, ...restored].map((record) => ({ kind, record })), { label: kind === 'printer' ? 'Add printer' : 'Add filament' });
  const renamed = result.records.flatMap((r, i) => r.name !== result.wanted[i] ? [noticeText('added_as_renamed', { name: r.name, wanted: result.wanted[i] })] : []);
  if (renamed.length) toast('added_renamed', { lines: renamed.join('\n') });
  return { keys: [...result.records, ...restored].map((r) => r.id), existing: result.existing, skipped: result.skipped, chosen: result.chosen, restored: restored.map((r) => r.name) };
}

export function catalogPrinterSetup(record: PrinterRecord): PrinterRecord {
  const feed = feedOf(record.model);
  return feed ? layoutSetup(record, [{ nozzle: 0 }]) : { ...record, slots: [{ id: `slot:${randomId()}`, filament: '' }] };
}

export async function addCatalogPrinters(choices: PrinterChoice[]): Promise<AddResult> {
  const variants = await Promise.all(choices.map(async ({ model, nozzle, firmware }) => defaultVariant(await catalogLine('printers', model.id), Number(nozzle), firmware)));
  return add('printer', (doc) => {
    const name = namer(doc.printers), live = doc.printers.filter((p) => !p.archived);
    const records: PrinterRecord[] = [], restored: PrinterRecord[] = [], existing: string[] = [], skipped: string[] = [], chosen: string[] = [], wanted: string[] = [];
    for (const [i, { vendor, model, nozzle, firmware, intent = 'add' }] of choices.entries()) {
      const variant = variants[i], diameter = Number(nozzle);
      const base = firmware && model.firmwares && firmware !== model.firmwares[0].id ? `${model.id} ${firmwareName(firmware)}` : model.id;
      const label = plain(nozzle === '0.4' || model.nozzles.length === 1 ? base : `${base} ${nozzle} nozzle`);
      if (!variant) throw new Error(`Hmm, ${model.id}${firmware ? ` (${firmwareName(firmware)})` : ''} doesn't have a ${nozzle} mm nozzle in the catalog.`);
      const on = (p: PrinterRecord) => p.model === model.id && p.nozzles[0]?.variant === variant.id;
      const have = intent === 'add' ? [...live, ...records, ...restored].find(on) : undefined;
      if (have) { skipped.push(label); existing.push(have.id); chosen.push(have.id); continue; }
      const removed = intent === 'another' ? undefined : doc.printers.filter((p) => p.archived && on(p))
        .sort((a, b) => (b.archived?.at ?? 0) - (a.archived?.at ?? 0))[0];
      if (removed) { const { archived: _a, ...back } = removed; restored.push({ ...back, name: name(back.name) }); chosen.push(back.id); continue; }
      const record = catalogPrinterSetup({ id: `profile:${randomId()}`, name: name(label), revision: 0, vendor, model: model.id,
        nozzles: Array.from({ length: Math.max(1, variant.extruders) }, () => ({ variant: variant.id, diameter })), slots: [] });
      records.push(record); chosen.push(record.id); wanted.push(label);
    }
    return { records, restored, existing, skipped, chosen, wanted };
  });
}
export async function addCatalogPrinter(vendor: string, model: CatalogModel, nozzle: string, intent: PrinterIntent = 'add', firmware?: string): Promise<string> {
  const { keys, skipped } = await addCatalogPrinters([{ vendor, model, nozzle, firmware, intent }]);
  if (!keys.length) throw new Error(`${skipped[0]} is already in your printers list.`);
  return keys[0];
}

export function addCatalogProducts(choices: ProductChoice[]): Promise<AddResult> {
  return add('filament', (doc) => {
    const name = namer(doc.filaments), live = doc.filaments.filter((f) => !f.archived);
    const records: FilamentRecord[] = [], existing: string[] = [], skipped: string[] = [], chosen: string[] = [], wanted: string[] = [];
    for (const { product, label } of choices) {
      const want = plain(label ?? product.id), have = [...live, ...records].find((f) => f.product === product.id);
      if (have) { skipped.push(want); existing.push(have.id); chosen.push(have.id); continue; }
      const record: FilamentRecord = { id: `profile:${randomId()}`, name: name(want), revision: 0, product: product.id };
      records.push(record); chosen.push(record.id); wanted.push(want);
    }
    return { records, existing, skipped, chosen, wanted };
  });
}
export async function addCatalogProduct(product: CatalogEntry, label?: string): Promise<string> {
  const { keys, skipped } = await addCatalogProducts([{ product, label }]);
  if (!keys.length) throw new Error(`${skipped[0]} is already in your filaments list.`);
  return keys[0];
}

export async function addPrinterOnVariant(model: CatalogModel, variantId: string, name?: string): Promise<string> {
  const line = await catalogLine('printers', model.id), variant = line.variants.find((v) => v.id === variantId);
  if (!variant) throw new Error(`Oops! We couldn't find ${variantId} in the catalog.`);
  const { keys, existing } = await addCatalogPrinters([{ vendor: model.vendor, model, nozzle: variant.nozzle, firmware: model.firmwares && firmwareOf(line, variant) }]);
  const key = keys[0] ?? existing[0];
  const printers = getState().records.printers, record = printers.find((p) => p.id === key);
  if (record && name && record.name !== name) await commitRecords([{ kind: 'printer', record: { ...record, name: namer(printers.filter((p) => p.id !== key))(name) } }], { label: 'Rename printer' });
  return key;
}
