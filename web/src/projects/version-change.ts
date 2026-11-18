import type { PresetType } from '../api/types';
import { OPTIONS } from '../options';
import { display, presetLabel } from '../profiles/format';
import { getState, type PlateObject } from '../state/store';
import { profileInfo } from '../profiles/view';
import type { Doc } from '../state/history';
import { documentStrip } from '../profiles/slots';

export interface VersionChange { key?: string; label: string; from?: string; to?: string }

export function profileName(type: PresetType, key: unknown): string {
  if (typeof key !== 'string' || !key) return '';
  const found = profileInfo(getState(), type, key).name;
  return found ? presetLabel(found) : '';
}
const PROFILE_TYPE: Record<string, PresetType> = { printer: 'machine', process: 'process', filament: 'filament' };

const shown = (v: unknown) => Array.isArray(v) ? v.join(', ') : String(v ?? '');
const name = (o: PlateObject) => o.name.replace(/\.(stl|obj|3mf|step|stp|drc)$/i, '');

function objectChanges(before: PlateObject[], after: PlateObject[], out: VersionChange[]) {
  const old = new Map(before.map((o) => [o.id, o]));
  for (const o of after) {
    const was = old.get(o.id);
    old.delete(o.id);
    if (!was) { out.push({ key: `o:${o.id}`, label: `Added ${name(o)}` }); continue; }
    if (was === o) continue;
    const what = was.instances !== o.instances ? (was.instances.length !== o.instances.length ? 'Copies of' : 'Moved')
      : was.rotX !== o.rotX || was.rotY !== o.rotY ? 'Rotated' : was.scale !== o.scale ? 'Scaled'
      : was.settings !== o.settings ? 'Settings of' : 'Edited';
    out.push({ key: `o:${o.id}:${what}`, label: `${what} ${name(o)}` });
  }
  for (const o of old.values()) out.push({ key: `o:${o.id}`, label: `Removed ${name(o)}` });
}

export function describeChange(before: Doc, after: Doc): VersionChange[] {
  const out: VersionChange[] = [];
  if (before.objects !== after.objects) objectChanges(before.objects, after.objects, out);
  if (before.projectPlates.length !== after.projectPlates.length)
    out.push({ key: 'plates', label: after.projectPlates.length > before.projectPlates.length ? 'Added plate' : 'Removed plate' });
  if (before.printer !== after.printer) out.push({ key: 'printer', label: 'Printer', from: profileName('machine', before.printer), to: profileName('machine', after.printer) });
  if (before.process !== after.process) out.push({ key: 'process', label: 'Process', from: profileName('process', before.process), to: profileName('process', after.process) });
  const a = documentStrip(before)?.filaments ?? [], b = documentStrip(after)?.filaments ?? [];
  if (shown(a) !== shown(b)) {
    if (a.length !== b.length) out.push({ key: 'filaments', label: b.length > a.length ? 'Added filament' : 'Removed filament' });
    else b.forEach((k, i) => k !== a[i] && out.push({ key: `filament:${i}`, label: b.length > 1 ? `Filament ${i + 1}` : 'Filament', from: profileName('filament', a[i]), to: profileName('filament', k) }));
  }
  if (before.dirty !== after.dirty && before.printer === after.printer && before.process === after.process) {
    for (const type of ['process', 'machine'] as const) {
      const a = before.dirty[type] ?? {}, b = after.dirty[type] ?? {};
      if (a === b) continue;
      const keys = [...new Set([...Object.keys(a), ...Object.keys(b)])].filter((k) => shown(a[k]) !== shown(b[k]));
      if (!keys.length) continue;
      for (const k of keys) out.push({ key: `${type}:${k}`, label: OPTIONS[k]?.label || k, from: shown(a[k]), to: shown(b[k]) });
    }
  }
  if (before.filamentEdits !== after.filamentEdits && !out.some((c) => c.key?.startsWith('filament'))) out.push({ key: 'filaments', label: 'Filament settings' });
  return out.slice(0, 8);
}

export function changeParts(c: VersionChange): { name: string; from?: string; to?: string } {
  if (c.from === undefined || c.to === undefined) return { name: c.label };
  const type = c.key && PROFILE_TYPE[c.key.split(':')[0]];
  if (type) {
    const to = profileName(type, c.to), from = profileName(type, c.from);
    return !to ? { name: `${c.label} changed` } : { name: c.label, from: from && from !== to ? from : undefined, to };
  }
  const setting = c.key?.split(':')[1], value = (v: string) => !v ? 'profile value' : (setting && OPTIONS[setting] ? display(setting, v) : v) || '–';
  return { name: c.label, from: value(c.from), to: value(c.to) };
}
