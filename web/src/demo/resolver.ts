import { exec } from 'virtual:demo-resolver';
import { compileResolver } from './resolver-wasm';
import rules from '../../../server/profile_rules.json?raw';
import variantKeys from '../../../server/filament_variant_keys.json?raw';
import feeds from '../../../server/printer_feeds.json?raw';
import schemas from '../../../server/schemas.json?raw';

type CatalogRow = { id: string; vendor?: string; brand?: string; type?: string };
export interface DemoCatalog { version: string; printers: CatalogRow[]; filaments: CatalogRow[]; prints: CatalogRow[] }
interface Resolver { configure(tables: string, printers: string, filaments: string, prints: string): string; answer(library: string, query: string): string; slotOffer(library: string, request: string): string }
declare global { var Go: new () => { importObject: WebAssembly.Imports; run(instance: WebAssembly.Instance): Promise<void> }; var pancakeResolver: Resolver | undefined }

const jsonl = (version: string, lines: unknown[]) => [{ schema: 'pancake.catalog.v1', version }, ...lines].map((l) => JSON.stringify(l)).join('\n');

let loading: Promise<Resolver> | undefined;
function load(options: unknown, catalog: DemoCatalog): Promise<Resolver> {
  return loading ??= (async () => {
    const module = compileResolver();
    await import(/* @vite-ignore */ exec);
    const go = new Go(), instance = await WebAssembly.instantiate(await module, go.importObject);
    void go.run(instance);
    const r = globalThis.pancakeResolver;
    if (!r) throw new Error('Oops! The demo resolver didn\'t start up.');
    const failed = r.configure(JSON.stringify({ rules: JSON.parse(rules), variantKeys: JSON.parse(variantKeys), feeds: JSON.parse(feeds), schemas: JSON.parse(schemas), options }),
      jsonl(catalog.version, catalog.printers), jsonl(catalog.version, catalog.filaments), jsonl(catalog.version, catalog.prints));
    if (failed) throw new Error(`Uh-oh, the demo resolver couldn't read its catalog: ${failed}`);
    return r;
  })().catch((e) => { loading = undefined; throw e; });
}

const reply = (raw: string): unknown => {
  const out = JSON.parse(raw) as { answer?: unknown; error?: string };
  if (out.error !== undefined) throw new Error(out.error);
  return out.answer;
};
export async function demoAnswer(library: unknown, query: unknown, options: unknown, catalog: DemoCatalog): Promise<unknown> {
  return reply((await load(options, catalog)).answer(JSON.stringify(library), JSON.stringify(query)));
}
export async function demoSlotOffer(library: unknown, request: unknown, options: unknown, catalog: DemoCatalog): Promise<unknown> {
  return reply((await load(options, catalog)).slotOffer(JSON.stringify(library), JSON.stringify(request)));
}
