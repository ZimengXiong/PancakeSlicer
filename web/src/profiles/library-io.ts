import { mutation, responseError } from '../api/access';
import { getState } from '../state/store';
import type { AnyRecord, RecordKind } from './records';
import { slotFilaments, slotIds } from './slots';
import { migrateLegacyHost } from '../printers/connections';
import { commitRecords } from './transactions';

export interface ImportRow {
  file: string; name: string; type?: 'machine' | 'filament' | 'process'; kind?: Exclude<RecordKind, 'calibration'>;
  record?: AnyRecord;
  status: 'new' | 'replace' | 'unchanged' | 'skip';
  basis?: string;
  notStored: string[]; note?: string;
  code?: 'import_invalid' | 'import_repeat';
  physical?: { printers: string[]; config: Record<string, string> };
  matches?: { id: string; name: string; revision: number; record: AnyRecord; edited?: string; differ: number }[];
}

export async function planImport(files: File[]): Promise<ImportRow[]> {
  const body = new FormData();
  for (const f of files) body.append('file', f, f.name);
  const r = await mutation('/api/library/import', { method: 'POST', body }, false, undefined, undefined, true);
  if (!r.ok) throw await responseError(r);
  return ((await r.json()) as { rows: ImportRow[] }).rows;
}

export async function applyImport(rows: ImportRow[]): Promise<{ profiles: number; connections: number; problems: string[] }> {
  const records = rows.filter((r) => r.status !== 'skip' && r.record && r.kind).map((r) => ({ kind: r.kind!, record: r.record! }));
  if (records.length) await commitRecords(records, { label: 'Import' });
  const hosts: { printer: string; config: Record<string, string> }[] = [];
  for (const r of rows) for (const printer of r.physical?.printers ?? []) hosts.push({ printer: printer.replace(/\s+@.*$/, ''), config: r.physical!.config });
  if (!hosts.length) return { profiles: records.length, connections: 0, problems: [] };
  if (getState().printerConnections.enabled === false) return { profiles: records.length, connections: 0, problems: ['Printer connections are off on this service; the print host settings were not migrated.'] };
  let connections = 0;
  const problems: string[] = [];
  for (const { printer, config } of hosts) {
    const record = getState().records.printers.find((p) => p.name === printer && !p.archived);
    if (!record) { problems.push(`${printer}: no saved printer of that name. Add the printer, then import its physical printer again.`); continue; }
    const problem = await migrateLegacyHost(record.id, record.name, config);
    if (problem) problems.push(problem); else connections++;
  }
  return { profiles: records.length, connections, problems };
}

export type ExportKind = 'printer_bundle' | 'filament_bundle' | 'printer' | 'filament' | 'process';
export interface ExportTarget { kind: 'printer' | 'filament' | 'process'; id: string; printer?: string }
type ExportBody = { kind: ExportKind; ids: string[] } | { kind: 'preset'; targets: ExportTarget[] }
  | { kind: 'current'; type: 'machine' | 'filament' | 'process'; slot: number };

export async function exportProfiles(body: ExportBody): Promise<void> {
  const s = getState(), selection = body.kind === 'current' ? { selection: { printer: s.printer, print: s.process, filaments: slotFilaments(s), slots: slotIds(s).map((slot) => slot ?? ''), layerHeight: 0,
    project: { machine: s.dirty.machine, process: s.dirty.process, filaments: slotFilaments(s).map((id) => s.filamentEdits[id] ?? {}) } } } : {};
  const r = await mutation('/api/library/export', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ ...body, ...selection, printer: s.printer }) });
  if (!r.ok) throw await responseError(r);
  const name = /filename="([^"]+)"/.exec(r.headers.get('Content-Disposition') ?? '')?.[1] ?? 'profiles.zip';
  const url = URL.createObjectURL(await r.blob());
  const a = document.createElement('a');
  a.href = url;
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 30_000);
}

export interface ProjectSettings {
  file: string; error?: string;
  printer: { name: string; variant?: string; library: string };
  print: { name: string; product?: string; library: string };
  filaments: { name: string; product?: string; library: string }[];
  machine: Record<string, import('../api/types').ConfigValue>; process: Record<string, import('../api/types').ConfigValue>;
  slots: Record<string, import('../api/types').ConfigValue>[];
  objects: { id: string; name: string; values: Record<string, import('../api/types').ConfigValue> }[];
  bedType?: string; notStored: string[];
  settings: boolean;
}
export async function decodeProjectSettings(file: File): Promise<ProjectSettings> {
  const body = new FormData();
  body.append('file', file, file.name.toLowerCase().endsWith('.3mf') ? file.name : `${file.name}.3mf`);
  const r = await mutation('/api/library/import', { method: 'POST', body }, false, undefined, undefined, true);
  if (!r.ok) throw await responseError(r);
  const project = ((await r.json()) as { projects: ProjectSettings[] }).projects[0];
  if (!project) throw new Error('Hmm, we couldn\'t find a project inside that file.');
  if (project.error) throw new Error(project.error);
  return project;
}
