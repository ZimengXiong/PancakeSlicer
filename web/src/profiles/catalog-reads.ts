import { catalogList, loadedList } from '../catalog/read';
import type { CatalogList, CatalogPrinterEntry } from '../catalog/types';

export type CatalogModel = CatalogPrinterEntry;
export interface CatalogVendor { id: string; name: string; models: CatalogModel[] }

const grouped = new WeakMap<CatalogList, CatalogVendor[]>();
export function printerVendors(list: CatalogList): CatalogVendor[] {
  let out = grouped.get(list);
  if (!out) {
    const vendors = new Map<string, CatalogVendor>();
    for (const m of list.entries as CatalogModel[]) {
      const v = vendors.get(m.vendor) ?? { id: m.vendor, name: m.vendorName, models: [] };
      v.models.push(m); vendors.set(m.vendor, v);
    }
    grouped.set(list, out = [...vendors.values()]);
  }
  return out;
}
export const printerCatalog = () => catalogList('printers').then(printerVendors);
export const loadedPrinterVendors = () => { const list = loadedList('printers'); return list && printerVendors(list); };

export const FIRST_SCREEN = 24;
const covers = new Map<string, HTMLImageElement>();
export function preloadCovers(vendors: CatalogVendor[], ids: Iterable<string>) {
  for (const id of new Set(ids)) preloadImages((vendors.find((v) => v.id === id)?.models.slice(0, FIRST_SCREEN) ?? []).flatMap((m) => m.cover ? [m.cover] : []));
}
export function preloadImages(urls: Iterable<string>) {
  for (const url of urls) {
    if (covers.has(url)) continue;
    const img = new Image();
    img.fetchPriority = 'low';
    img.src = url;
    covers.set(url, img);
    img.decode().catch(() => covers.delete(url));
  }
}
