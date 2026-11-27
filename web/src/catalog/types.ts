import type { Config } from '../api/types';

export type CatalogKind = 'printers' | 'filaments' | 'prints';

export interface CatalogSource { name: string; vendor: string; version?: string; file?: string; filament_id?: string; native_support_capable?: boolean }

export interface CatalogVariant { id: string; nozzle: string; default: boolean; defaultPrint?: string; extruders: number; nozzles: { diameter: string }[]; values: Config; source: CatalogSource }
export interface CatalogPrinter { id: string; vendor: string; vendor_name?: string; model: Record<string, unknown>; base: Config; variants: CatalogVariant[] }

export interface CatalogTuning { fits: string[]; values: Config; source: CatalogSource }
export interface CatalogProduct { id: string; brand?: string; type?: string; base: Config; tunings: CatalogTuning[] }

export type CatalogLine<K extends CatalogKind> = K extends 'printers' ? CatalogPrinter : CatalogProduct;
export interface CatalogEntry { id: string; brand?: string; type?: string; vendor?: string }
export interface CatalogFirmware { id: string; nozzles: string[] }
export interface CatalogPrinterEntry extends CatalogEntry { vendor: string; vendorName: string; family?: string; cover?: string; bed?: string; extruders: number; nozzles: string[]; firmwares?: CatalogFirmware[] }
export interface CatalogList { version: string; entries: CatalogEntry[] }
