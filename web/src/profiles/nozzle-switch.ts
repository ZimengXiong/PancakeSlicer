import type { Config, SystemPreset, UserPreset } from '../api/types';
import { sameValue } from '../state/value-equality';
import { getState, toast, type State } from '../state/store';
import { commitProfileRecords } from './transactions';
import { findProfile, profileKey } from './identity';
import { resolveProfile } from './model';
import { printerCatalog, type CatalogModel } from './catalog-reads';
import { printDefaults } from './catalog-add';
import { loadFilamentVariants, loadMachinePresets } from './catalog-startup';
import { capturePrinterMaterials, reconcilePrinterSelection, selectionNotice } from './material-library';
import { availableHeights, effectiveLayerHeight } from './context';
import { SEP } from '../separator';

export interface NozzleModel { vendor: string; model: CatalogModel; nozzle: string }
export async function nozzleModel(base: string): Promise<NozzleModel | undefined> {
  for (const v of await printerCatalog()) for (const model of v.models)
    for (const [nozzle, machine] of Object.entries(model.machines ?? {})) if (machine === base) return { vendor: v.id, model, nozzle };
  return undefined;
}

const PRINTER_OWNED = /_gcode$|^printable_|^bed_|^curr_bed_type$|host|^thumbnails|^printer_notes$/;
const nozzleDependent = (key: string, from: Config, to: Config) => key === 'nozzle_diameter' || !PRINTER_OWNED.test(key) && !sameValue(key, from[key], to[key]);
const report = (key: string, dropped: string[]) => { if (key !== 'nozzle_diameter') dropped.push(key); };

export interface NozzlePlan { record: UserPreset; dropped: string[] }
export function planNozzle(record: UserPreset, from: SystemPreset, to: SystemPreset, oldDefaults: Config, newDefaults: Config): NozzlePlan {
  const dropped: string[] = [], overlay: Config = {}, baseline: Record<string, Config[string] | null> = {};
  for (const [key, value] of Object.entries(record.overlay ?? record.overrides ?? {})) {
    if (nozzleDependent(key, from.config, to.config)) { report(key, dropped); continue; }
    overlay[key] = value; baseline[key] = to.config[key] ?? null;
  }
  const prior = (record as { printOverrides?: Config }).printOverrides ?? {}, printOverrides: Config = { ...newDefaults };
  for (const [key, value] of Object.entries(prior)) {
    if (sameValue(key, value, oldDefaults[key])) continue;
    if (!sameValue(key, oldDefaults[key], newDefaults[key])) { dropped.push(key); continue; }
    printOverrides[key] = value;
  }
  const { snapshot: _snapshot, ...rest } = record;
  return { dropped, record: { ...rest, inherits: to.name, base: { name: to.name, engineName: to.engine_base ?? to.name, version: '', vendor: to.vendor },
    overlay, overrides: overlay, baseline, ...(Object.keys(printOverrides).length ? { printOverrides } : {}) } as UserPreset };
}

const label = (key: string) => key.replace(/_/g, ' ');
export async function switchNozzle(printerKey: string, nozzle: string): Promise<string[]> {
  const before = getState(), record = findProfile(before, 'machine', printerKey), from = resolveProfile(before, 'machine', printerKey).base;
  if (!record || !from) throw new Error('Oops! That printer isn\'t available right now.');
  const found = await nozzleModel(from.name);
  if (!found) throw new Error(`${record.name} doesn't have any nozzle presets in the catalog.`);
  const target = found.model.machines[nozzle];
  if (!target) throw new Error(`${found.model.name} doesn't have a ${nozzle} mm nozzle preset.`);
  if (target === from.name && (record.overlay ?? record.overrides ?? {}).nozzle_diameter === undefined) return [];
  await loadMachinePresets(found.vendor, [target]);
  await loadFilamentVariants(getState().userPresets, [target]);
  const to = resolveProfile({ ...getState(), userPresets: [] }, 'machine', target).base;
  if (!to) throw new Error(`Oops! Couldn't load the ${nozzle} mm nozzle preset.`);
  const [oldDefaults, newDefaults] = await Promise.all([printDefaults({ ...found, nozzle: found.nozzle }), printDefaults({ ...found, nozzle })]);
  const plan = planNozzle(record, from, to, oldDefaults ?? {}, newDefaults ?? {});
  const projectDropped: string[] = [];
  await commitProfileRecords([plan.record], { apply: (s) => follow(s, plan.record, from, to, projectDropped) });
  const dropped = [...new Set([...plan.dropped, ...projectDropped])];
  const after = getState(), notice = after.printer === printerKey && after.lastProfileTransition ? selectionNotice(after, after.lastProfileTransition) : '';
  toast([`${record.name}: ${nozzle} mm nozzle`, dropped.length && `Not carried over: ${dropped.map(label).join(', ')}`, notice].filter(Boolean).join(SEP),
    { label: 'Undo', run: () => void undoNozzle(printerKey, record) }, 12000, dropped.length || notice ? 'warning' : undefined);
  return dropped;
}

function follow(s: State, rebased: UserPreset, from: SystemPreset, to: SystemPreset, dropped: string[]): Partial<State> {
  const key = profileKey(rebased);
  if (s.printer !== key) return {};
  const strip = (config: Config) => Object.fromEntries(Object.entries(config).filter(([k]) => !nozzleDependent(k, from.config, to.config) || (report(k, dropped), false)));
  const next: State = { ...s, userPresets: s.userPresets.map((p) => profileKey(p) === key ? rebased : p), dirty: { ...s.dirty, machine: strip(s.dirty.machine) },
    projectPresets: s.projectPresets && { ...s.projectPresets, machine: strip(s.projectPresets.machine) } };
  const patch = reconcilePrinterSelection(s, next).patch, done: State = { ...next, ...patch };
  if (done.processVariant && !availableHeights(done).some((h) => Math.abs(h - effectiveLayerHeight(done)) < 5e-4)) done.processVariant = null;
  return { ...patch, dirty: done.dirty, projectPresets: done.projectPresets, processVariant: done.processVariant, printerLibrary: capturePrinterMaterials(done) };
}

async function undoNozzle(printerKey: string, previous: UserPreset) {
  const current = findProfile(getState(), 'machine', printerKey);
  if (!current) { toast('Can\'t undo because that printer was removed from your library.'); return; }
  const from = resolveProfile(getState(), 'machine', printerKey).base, to = resolveProfile({ ...getState(), userPresets: [] }, 'machine', previous.base?.name ?? previous.inherits).base;
  try {
    const restored = { ...previous, revision: current.revision };
    await commitProfileRecords([restored], { apply: (s) => from && to ? follow(s, restored, from, to, []) : {} });
    toast(`${previous.name}: nozzle restored`);
  } catch (e) { toast(e instanceof Error ? e.message : String(e)); }
}
