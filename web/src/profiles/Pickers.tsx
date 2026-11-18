import { modelLabel, nozzleField, printerCover, printerTrigger, rowDetail, vendorField, vendorLabel } from './metadata';
import { openLibrary } from '../navigation';
import type { PresetType } from '../api/types';
import { getState, resolvePreset, useStore } from '../state/store';
import { Picker, type PickerAction, type PickerGroup, type PickerItem } from '../components/picker/Picker';
import { requestPreset } from '../components/PresetCombo';
import { filamentColor } from '../plater/filamentColor';
import { FilamentColourMenu } from './FilamentColourMenu';
import { listProfiles, type PickerEntry } from './pickerSource';
import { pickerStatus } from '../printers/connections';
import { findProfile } from './identity';
import { SEP } from '../separator';
import { slotFilaments } from './slots';

const first = (v: unknown) => String([v].flat()[0] ?? '');
const shortName = (name: string) => name.replace(/\s*@.*$/, '') || name;
export const presets = (type: PresetType, name?: string): PickerAction => ({ label: `Manage ${type === 'machine' ? 'printers' : type === 'filament' ? 'filaments' : 'print settings'}`, run: () => openLibrary(type, { name }) });


function brandName(name: string, vendor: string) {
  const brand = vendor || 'Generic', short = shortName(name);
  const word = brand.split(/\s+/)[0];
  return { brand, name: short.toLowerCase().startsWith(word.toLowerCase() + ' ') ? short.slice(word.length + 1) : short };
}

const MATERIALS = ['PLA', 'PETG', 'ABS', 'ASA', 'TPU', 'PC', 'PA', 'PVA'];
const RARE = /^(PVA|BVOH|HIPS|EVA|COPE|PVB|SUPPORT|OTHER)$/;
export const materialRank = (t: string) => { const i = MATERIALS.indexOf(t.toUpperCase()); return i >= 0 ? i : RARE.test(t.toUpperCase()) ? 99 : 50; };
const material = (type: string) => (type.toUpperCase().split(/[-+\s]/)[0] || 'Other').replace(/^PA\d+$/, 'PA');

export function currentEntry(type: PresetType, key: string | undefined, entries: readonly PickerEntry[]): PickerEntry | undefined {
  if (!key || entries.some((e) => e.key === key)) return undefined;
  const r = resolvePreset(getState(), type, key);
  return r.name ? { key, name: r.name, vendor: r.base?.vendor ?? '', material: '', mine: true } : undefined;
}
export const UNAVAILABLE = 'Unavailable';

export function usePickerPending(resolved: boolean): boolean {
  const load = useStore((s) => s.profileLibraryLoad.state), api = useStore((s) => !!s.api);
  return !api || load === 'loading' || load === 'failed' && !resolved;
}
export const PickerSkeleton = () => <><span className="pk-name" aria-label="Loading"><span className="skel-line pk-skel-name"/></span><span className="pk-sub"><span className="skel-line pk-skel-sub"/></span></>;

const listCache = new Map<string, { at: unknown; value: PickerGroup[] }>();
export const sameStateGroups = (key: string, build: () => PickerGroup[]) => (): PickerGroup[] => {
  const at = getState(), hit = listCache.get(key);
  if (hit && hit.at === at) return hit.value;
  const value = build(); listCache.set(key, { at, value }); return value;
};

const addFilament: PickerAction = { label: 'Add filament…', run: () => openLibrary('filament', { add: true }) };

function filamentGroups(slot: number | null): PickerGroup[] {
  const s = getState();
  let entries = listProfiles('filament');
  const hidden = slot === null ? undefined : currentEntry('filament', slotFilaments(s)[slot], entries);
  if (hidden) entries = [...entries, hidden];
  const others = new Map<string, { n: number; colour: string }[]>();
  slotFilaments(s).forEach((name, i) => { if (i !== slot && name) others.set(name, [...(others.get(name) ?? []), { n: i + 1, colour: filamentColor(s, i + 1) }]); });
  const item = (e: PickerEntry): PickerItem => {
    const brand = e.vendor || 'Generic', b = brandName(e.name, brand);
    const label = e.mine ? e.name : b.name;
    return { id: e.key, label, sub: rowDetail(label, [vendorField(brand)]) || undefined, facet: brand, disabled: e.reason ?? false, search: `${brand} ${e.material} ${e.name}`, slots: others.get(e.key) };
  };
  const seen = new Map<string, number>();
  for (const e of entries) { const k = item(e).label + e.vendor; seen.set(k, (seen.get(k) ?? 0) + 1); }
  const types = new Map<string, PickerItem[]>(), loaded: PickerItem[] = [];
  for (const e of entries) {
    const it = item(e), at = e.name.match(/@\s*(.+)$/)?.[1];
    if (e === hidden) it.meta = UNAVAILABLE;
    if (!e.mine && at && !/system/i.test(at) && seen.get(it.label + e.vendor)! > 1) it.label += `${SEP}${at.replace(/^BBL\s+/, '')}`;
    const type = material(e.material);
    types.set(type, [...(types.get(type) ?? []), it]);
    if (it.slots) loaded.push(it);
  }
  const rank = materialRank;
  const groups: PickerGroup[] = [];
  if (loaded.length > 1) groups.push({ id: 'loaded', title: 'Loaded', items: [...loaded].sort((a, b) => a.slots![0].n - b.slots![0].n), filter: true });
  for (const type of [...types.keys()].sort((a, b) => rank(a) - rank(b) || a.localeCompare(b)))
    groups.push({ id: `t:${type}`, title: type, items: types.get(type)!.sort((a, b) => a.facet!.localeCompare(b.facet!) || a.label.localeCompare(b.label)) });
  return groups;
}

export function FilamentPicker({ slot }: { slot: number }) {
  const name = useStore((s) => slotFilaments(s)[slot] ?? '');
  useStore((s) => s.filamentEdits);
  useStore((s) => s.records);
  const s = getState();
  const resolved = resolvePreset(s, 'filament', name), c = resolved.config, label = resolved.name ?? 'Unavailable filament';
  const b = brandName(label, resolved.base?.vendor ?? ''), pending = usePickerPending(!!resolved.name);
  return (
    <span className="pk-slot">
      <FilamentColourMenu slot={slot}/>
      <Picker label={`Filament ${slot + 1}`} value={name} sections memory="orca.web.picker.filament" facetHeads groups={sameStateGroups(`filament:${slot}`, () => filamentGroups(slot))}
        empty={listProfiles('filament').length ? 'No filaments here yet' : 'No filaments in your library yet'}
        onPick={(id) => id !== name && requestPreset('filament', slot, id)} footer={[addFilament, presets('filament', name)]}>
        {pending ? <PickerSkeleton/> : !name ? <span className="pk-name pk-empty">Pick a filament…</span> : <><span className="pk-type">{first(c.filament_type) || '—'}</span>
        <span className="pk-brand">{b.brand}</span>
        <span className="pk-name">{label}</span></>}
      </Picker>
    </span>
  );
}


const POPULAR = [/bambu|bbl/i, /prusa/i, /creality/i, /anycubic/i, /elegoo/i, /voron/i, /qidi/i, /flashforge/i];
const vendorRank = (v: string) => { const i = POPULAR.findIndex((r) => r.test(v)); return i >= 0 ? i : /custom|generic/i.test(v) ? 99 : 50; };

const byVendorRail = (byVendor: Map<string, PickerItem[]>): PickerGroup[] =>
  [...byVendor].sort((a, b) => vendorRank(a[0]) - vendorRank(b[0]) || b[1].length - a[1].length || a[0].localeCompare(b[0]))
    .map(([v, items]) => ({ id: `v:${v}`, title: v, items: items.sort((a, b) => a.label.localeCompare(b.label)) }));

export { printerShortName } from './metadata';

export function PrinterPicker() {
  const printer = useStore((s) => s.printer);
  useStore((s) => s.records);
  const connections = useStore((s) => s.printerConnections);
  const profileGroups = (): PickerGroup[] => {
    const byVendor = new Map<string, PickerItem[]>();
    for (const e of listProfiles('machine')) {
      const v = vendorLabel({}, e.vendor);
      byVendor.set(v, [...(byVendor.get(v) ?? []), { id: e.key, label: e.mine ? e.name : shortName(e.name), sub: rowDetail(e.name, [{ value: modelLabel({}, e.vendor, e.model) }, nozzleField(e.nozzle ? String(e.nozzle) : '')], [v]) || undefined, search: e.name, image: printerCover(e.vendor, {}, e.model) ?? null, status: pickerStatus(connections, e.key) }]);
    }
    return byVendorRail(byVendor);
  };
  const selected = resolvePreset(getState(), 'machine', printer), pending = usePickerPending(!!selected.name);
  const record = useStore((s) => s.records.printers.find((p) => p.id === s.printer));
  const lines = record?.nozzles[0] ? printerTrigger({ vendor: record.vendor, model: record.model, variant: record.nozzles[0].variant, nozzle: record.nozzles[0].diameter, ...(record.nozzles.length > 1 ? { nozzles: record.nozzles.map((n) => n.diameter) } : {}) }) : null;
  const removed = useStore((s) => { const p = findProfile(s, 'machine', s.printer); return p?.archived ? p.name : ''; });
  return (
    <Picker label="Printer" value={printer} memory="orca.web.picker.printers" groups={sameStateGroups('printer', profileGroups)} onPick={(name) => name !== printer && requestPreset('machine', 0, name)}
      footer={[{ label: 'Add printer', run: () => openLibrary('machine', { add: true }) }, presets('machine', printer)]}>
      {removed ? <><span className="pk-name pk-empty">Choose printer</span><span className="pk-sub">{removed} was removed</span></>
        : pending ? <PickerSkeleton/> : <span className="pk-name" title={lines ? record!.nozzles[0].variant : undefined}>{lines?.name || 'Choose printer'}</span>}
      {!removed && !pending && lines?.sub && <span className="pk-sub">{lines.sub}</span>}
    </Picker>
  );
}

export function PresetPicker({ type, slot = 0 }: { type: PresetType; slot?: number }) {
  if (type === 'machine') return <PrinterPicker />;
  return <FilamentPicker slot={slot} />;
}
