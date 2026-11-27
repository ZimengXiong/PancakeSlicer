import { normalizeConfig } from '../config-values';
import { createMachine, useMachine, type Machine } from '../flow/fsm';
import { responseError } from '../api/access';
import type { CatalogEntry, CatalogKind, CatalogLine, CatalogList, CatalogPrinter, CatalogProduct, CatalogTuning, CatalogVariant } from './types';

type ReadState = 'idle' | 'loading' | 'ready' | 'failed';
interface ReadContext<T> { path: string; value: T | null; error: string | null; request: number }
type ReadEvent<T> = { type: 'READ' } | { type: 'RETRY' } | { type: 'LOADED'; request: number; value: T } | { type: 'FAILED'; request: number; error: string };
export type CatalogRead<T> = Machine<ReadState, ReadEvent<T>, ReadContext<T>>;

const mine = (c: { request: number }, e: { request: number }) => e.request === c.request;
const late = {} as const;
function createRead<T>(path: string, decode: (body: unknown) => T): CatalogRead<T> {
  const fetchOnce = async (m: CatalogRead<T>, request: number) => {
    try {
      const r = await fetch(path, { cache: 'no-cache' });
      if (!r.ok) throw await responseError(r);
      m.send({ type: 'LOADED', request, value: decode(await r.json()) });
    } catch (e) { m.send({ type: 'FAILED', request, error: e instanceof Error ? e.message : String(e) }); }
  };
  const begin = { target: 'loading', action: (c: ReadContext<T>) => ({ request: c.request + 1, error: null }) } as const;
  return createMachine<ReadState, ReadEvent<T>, ReadContext<T>>({
    id: 'catalog-read', initial: 'idle', strict: true, context: { path, value: null, error: null, request: 0 },
    states: {
      idle: { on: { READ: begin, RETRY: begin, LOADED: late, FAILED: late } },
      loading: { entry: (c, _, m) => { void fetchOnce(m, c.request); },
        on: { READ: late, RETRY: late,
          LOADED: [{ target: 'ready', guard: mine, action: (_, e) => ({ value: e.value, error: null }) }, late],
          FAILED: [{ target: 'failed', guard: mine, action: (_, e) => ({ error: e.error }) }, late] } },
      ready: { on: { READ: late, RETRY: late, LOADED: late, FAILED: late } },
      failed: { on: { READ: late, RETRY: begin, LOADED: late, FAILED: late } },
    },
  });
}

const values = (v: Record<string, unknown> | undefined) => normalizeConfig((v ?? {}) as never, true);
const tuning = (t: CatalogTuning): CatalogTuning => ({ ...t, values: values(t.values) });
const decodeProduct = (b: unknown): CatalogProduct => { const p = b as CatalogProduct; return { ...p, base: values(p.base), tunings: (p.tunings ?? []).map(tuning) }; };
const decodePrinter = (b: unknown): CatalogPrinter => { const p = b as CatalogPrinter; return { ...p, base: values(p.base), variants: p.variants.map((v) => ({ ...v, values: values(v.values) })) }; };
const decodeList = (kind: CatalogKind) => (b: unknown): CatalogList => { const l = b as { version: string } & Record<string, CatalogEntry[]>; return { version: l.version, entries: l[kind] ?? [] }; };

const reads = new Map<string, CatalogRead<unknown>>();
function read<T>(path: string, decode: (b: unknown) => T): CatalogRead<T> {
  let m = reads.get(path) as CatalogRead<T> | undefined;
  if (!m) { reads.set(path, (m = createRead(path, decode)) as CatalogRead<unknown>); m.send({ type: 'READ' }); }
  return m;
}
const linePath = (kind: CatalogKind, id: string) => `/api/catalog/${kind}/${encodeURIComponent(id)}`;
export const lineRead = <K extends CatalogKind>(kind: K, id: string) =>
  read<CatalogLine<K>>(linePath(kind, id), (kind === 'printers' ? decodePrinter : decodeProduct) as (b: unknown) => CatalogLine<K>);
export const listRead = (kind: CatalogKind) => read<CatalogList>(`/api/catalog/${kind}`, decodeList(kind));

export function settle<T>(m: CatalogRead<T>): Promise<T> {
  if (m.get().value === 'failed') m.send({ type: 'RETRY' });
  return new Promise((resolve, reject) => {
    const check = () => {
      const { value, context } = m.get();
      if (value === 'ready') { stop(); resolve(context.value as T); } else if (value === 'failed') { stop(); reject(new Error(context.error ?? 'Oops! Couldn\'t read the catalog.')); }
    };
    const stop = m.subscribe(check); check();
  });
}
export const catalogLine = <K extends CatalogKind>(kind: K, id: string) => settle(lineRead(kind, id));
export const catalogList = (kind: CatalogKind) => settle(listRead(kind));
export const loadedLine = <K extends CatalogKind>(kind: K, id: string): CatalogLine<K> | undefined =>
  (reads.get(linePath(kind, id))?.get().context.value ?? undefined) as CatalogLine<K> | undefined;

export const loadedList = (kind: CatalogKind): CatalogList | undefined =>
  (reads.get(`/api/catalog/${kind}`)?.get().context.value ?? undefined) as CatalogList | undefined;

export function useCatalogRead<T>(m: CatalogRead<T>): { state: ReadState; value: T | null; error: string | null; retry(): void } {
  const snap = useMachine(m);
  return { state: snap.value, value: snap.context.value as T | null, error: snap.context.error, retry: () => m.send({ type: 'RETRY' }) };
}

export const tuningFor = (product: CatalogProduct, variant: string) => product.tunings.find((t) => t.fits.includes(variant));
export function firmwareOf(printer: CatalogPrinter, variant: CatalogVariant): string {
  const flavor = String(variant.values.gcode_flavor ?? printer.base.gcode_flavor ?? '');
  return flavor.startsWith('marlin') ? 'marlin' : flavor;
}
const FIRMWARE_NAMES: Record<string, string> = { marlin: 'Marlin', klipper: 'Klipper', reprapfirmware: 'RepRapFirmware' };
export const firmwareName = (id: string) => FIRMWARE_NAMES[id] ?? id.charAt(0).toUpperCase() + id.slice(1);
export function defaultVariant(printer: CatalogPrinter, nozzle: number, firmware?: string): CatalogVariant | undefined {
  const sized = printer.variants.filter((v) => Math.abs(Number(v.nozzle) - nozzle) < 1e-6 && (!firmware || firmwareOf(printer, v) === firmware));
  return sized.find((v) => v.default) ?? (firmware ? sized[0] : undefined);
}
