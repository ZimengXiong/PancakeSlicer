import { failureText } from '../api/access';
import { Check } from './Check';
import { useRef, useState } from 'react';
import type { PresetType } from '../api/types';
import { applyImport, exportProfiles, planImport, type ExportKind, type ImportRow } from '../profiles/library-io';
import { setState, toast, useStore } from '../state/store';
import { Dropdown } from './Dropdown';
import { Modal, ModalBody, ModalFooter, ModalTitle } from './Modal';
import { createMachine, useMachine } from '../flow/fsm';
import { notice, noticeText, type NoticeKind } from '../notice-codes';

const TYPE_NAME: Record<PresetType, string> = { machine: 'Printer', filament: 'Filament', process: 'Process' };
const ACCEPT = '.json,.zip,.bbscfg,.orca_printer,.orca_filament,.orca_bundle';
const close = () => setState({ presetUx: null });

type Row = ImportRow & { checked: boolean; result?: string; ok?: boolean;  added?: ImportRow['record']; addedNote?: string;
   pick?: string };
type Match = NonNullable<ImportRow['matches']>[number];
function editedAt(m: Match): string {
  if (!m.edited) return `revision ${m.revision}`;
  const at = new Date(m.edited), time = at.toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' });
  return `edited ${at.toDateString() === new Date().toDateString() ? time : `${at.toLocaleDateString([], { month: 'short', day: 'numeric' })} ${time}`}`;
}
const differ = (m: Match) => m.differ === 0 ? 'no settings differ' : m.differ === 1 ? '1 setting differs' : `${m.differ} settings differ`;
const skipped = (r: ImportRow) => ({ text: noticeText(r.code!, { note: r.note }), kind: notice(r.code!).kind });
const KIND_CLASS: Partial<Record<NoticeKind, string>> = { error: 'np-error', warning: 'np-warn-text' };
const importable = (r: ImportRow) => r.status !== 'unchanged' && r.status !== 'skip' && !!(r.record || r.physical);

type ImportState = 'empty' | 'reading' | 'review' | 'importing' | 'imported';
type ImportEvent = { type: 'FILES' } | { type: 'PARSED'; request: number; rows: Row[] } | { type: 'READ_FAIL'; request: number; error: string }
  | { type: 'ROWS'; rows: Row[] } | { type: 'IMPORT' } | { type: 'IMPORTED'; rows: Row[] } | { type: 'IMPORT_FAIL'; rows: Row[] };
interface ImportContext { request: number; rows: Row[] | null; error: string | null }
const files = { target: 'reading', action: (c: ImportContext) => ({ request: c.request + 1, error: null }) } as const;
const createImport = () => createMachine<ImportState, ImportEvent, ImportContext>({
  id: 'preset-import', initial: 'empty', strict: true, context: { request: 0, rows: null, error: null },
  states: {
    empty: { on: { FILES: files } },
    reading: { on: {
      PARSED: { target: 'review', guard: (c, e) => e.request === c.request, action: (_, e) => ({ rows: e.rows }) },
      READ_FAIL: [{ target: 'review', guard: (c, e) => e.request === c.request && !!c.rows, action: (_, e) => ({ error: e.error }) },
        { target: 'empty', guard: (c, e) => e.request === c.request, action: (_, e) => ({ error: e.error }) }],
    } },
    review: { on: { FILES: files, ROWS: { action: (_, e) => ({ rows: e.rows }) },
      IMPORT: { target: 'importing', guard: (c) => !!c.rows?.some((r) => r.checked && importable(r)), action: () => ({ error: null }) } } },
    importing: { on: { IMPORTED: { target: 'imported', action: (_, e) => ({ rows: e.rows }) }, IMPORT_FAIL: { target: 'review', action: (_, e) => ({ rows: e.rows }) } } },
    imported: { on: { FILES: files } },
  },
});

export function ImportDialog() {
  const [machine] = useState(createImport);
  const { value, context: { rows, error } } = useMachine(machine);
  const busy = value === 'reading' || value === 'importing', done = value === 'imported';
  const setRows = (next: Row[]) => machine.send({ type: 'ROWS', rows: next });
  const [drag, setDrag] = useState(false);
  const input = useRef<HTMLInputElement>(null);

  const read = async (picked: FileList | File[]) => {
    const list = [...picked];
    if (!list.length || !machine.can({ type: 'FILES' })) return;
    machine.send({ type: 'FILES' });
    const request = machine.get().context.request;
    try {
      machine.send({ type: 'PARSED', request, rows: (await planImport(list)).map((p) => ({ ...p, checked: importable(p), added: p.record, addedNote: p.note, pick: '' })) });
    } catch (e) { machine.send({ type: 'READ_FAIL', request, error: failureText('Oops! Couldn\'t read those files', e) }); }
  };

  const run = async () => {
    if (!rows || !machine.can({ type: 'IMPORT' })) return;
    machine.send({ type: 'IMPORT' });
    const chosen=rows.filter(r=>r.checked && importable(r));
    try {
      const result=await applyImport(chosen);
      machine.send({ type: 'IMPORTED', rows: rows.map(row=>chosen.includes(row) ? {...row,ok:true,result:row.status === 'replace' ? 'replaced' : 'imported'} : row) });
      toast('profiles_imported', { profiles: result.profiles, connections: result.connections ?? 0, problems: result.problems.join('\n') });
    }catch(e){machine.send({ type: 'IMPORT_FAIL', rows: rows.map(row=>chosen.includes(row) ? {...row,ok:false,result:e instanceof Error ? e.message : String(e)} : row) });}
  };

  const count = rows?.filter((r) => r.checked && importable(r)).length ?? 0;
  return (
    <Modal size="l" className="import-dialog" label="Import profiles" onClose={close}>
      <ModalTitle title="Import profiles" onClose={close} />
      <ModalBody className={`import-drop ${drag ? 'drag' : ''}`}>
        <div className="import-zone" onDragOver={(e) => { e.preventDefault(); setDrag(true); }} onDragLeave={() => setDrag(false)}
          onDrop={(e) => { e.preventDefault(); e.stopPropagation(); setDrag(false); void read(e.dataTransfer.files); }}>
          <span>Drop .json, .zip or .orca_* files here</span>
          <button type="button" className="btn" onClick={() => input.current?.click()} disabled={busy}>Choose files…</button>
          <input ref={input} type="file" multiple hidden accept={ACCEPT} aria-label="Profile files" onChange={(e) => { if (e.target.files) void read(e.target.files); e.target.value = ''; }} />
        </div>
        {busy && <div role="status">{value === 'importing' ? 'Importing…' : 'Reading…'}</div>}
        {error && <p className="modal-error" role="alert">{error}</p>}
        {rows && !rows.length && <p>Hmm, we didn't find any usable profiles inside those files.</p>}
        {rows && rows.length > 0 && (
          <div className="diff-scroll">
            <table className="np-table">
              <thead><tr><th><Check aria-label="Select all" disabled={done || busy} checked={rows.every((r) => r.checked || !importable(r))}
                onCheckedChange={(next) => setRows(rows.map((r) => ({ ...r, checked: importable(r) && next })))} /></th>
                <th>Preset</th><th>Type</th><th>Starts from</th><th>{done ? 'Result' : 'Action'}</th></tr></thead>
              <tbody>{rows.map((r, i) => (
                <tr key={`${r.file}:${i}`}>
                  <td><Check aria-label={`Import ${r.name}`} disabled={!importable(r) || done || busy} checked={r.checked}
                    onCheckedChange={(next) => setRows(rows.map((x, j) => (j === i ? { ...x, checked: next } : x)))} /></td>
                  <td>{r.name}</td>
                  <td>{r.type ? TYPE_NAME[r.type] : r.physical ? 'Physical printer' : '—'}</td>
                  <td>{r.basis ?? ''}</td>
                  <td className={r.ok === false ? 'np-error' : r.status === 'skip' ? KIND_CLASS[skipped(r).kind] : r.status === 'replace' && !done ? 'np-warn-text' : undefined}>
                    {done || r.ok === false ? (r.result ?? (r.status === 'unchanged' ? 'unchanged' : r.status === 'skip' ? skipped(r).text : 'not selected'))
                      : r.status === 'unchanged' ? 'unchanged' : r.status === 'skip' ? skipped(r).text : r.pick || r.matches?.length ? r.note : `${r.status === 'replace' ? 'overwrite' : 'import'}${r.note ? ` (${r.note})` : ''}`}
                    {r.matches && r.matches.length > 0 && !done && <Dropdown className="combo" aria-label={`What ${r.name} does`} value={r.pick ?? ''}
                      options={[{ value: '', label: 'Add as new' }, ...r.matches.map((m) => ({ value: m.id, label: `Replace ${m.name}, ${editedAt(m)}, ${differ(m)}` }))]}
                      onValueChange={(pick) => setRows(rows.map((x, j) => { if (j !== i) return x; const m = x.matches?.find((y) => y.id === pick);
                        const status = !m ? 'new' as const : m.differ === 0 && m.name === x.name ? 'unchanged' as const : 'replace' as const;
                        const note = !m ? x.addedNote : `Replaces ${m.name} (${editedAt(m)})`;
                        return { ...x, pick, record: m ? m.record : x.added, status, note, checked: status !== 'unchanged' }; }))}/>}
                    {r.notStored.length > 0 && <details className="import-not-stored"><summary>{r.notStored.length} not stored</summary><ul>{r.notStored.map((k) => <li key={k}>{k}</li>)}</ul></details>}
                  </td>
                </tr>
              ))}</tbody>
            </table>
          </div>
        )}
      </ModalBody>
      <ModalFooter>
        <button type="button" className="btn" onClick={close}>{done ? 'Close' : 'Cancel'}</button>
        {!done && <button type="button" className="btn-primary" disabled={busy || !count} onClick={() => void run()}>{!count ? 'Import' : count === 1 ? 'Import 1 profile' : `Import ${count} profiles`}</button>}
      </ModalFooter>
    </Modal>
  );
}

const KINDS: [ExportKind, string][] = [
  ['printer_bundle', 'Printer bundle (.orca_printer)'], ['filament_bundle', 'Filament bundle (.orca_filament)'],
  ['printer', 'Printers (.zip)'], ['filament', 'Filaments (.zip)'], ['process', 'Print settings (.zip)'],
];

type ExportState = 'choosing' | 'exporting' | 'failed';
type ExportEvent = { type: 'EXPORT' } | { type: 'DONE' } | { type: 'FAIL'; error: string } | { type: 'EDIT' };
const createExport = () => createMachine<ExportState, ExportEvent, { error: string | null }>({
  id: 'preset-export', initial: 'choosing', strict: true, context: { error: null },
  states: {
    choosing: { on: { EXPORT: { target: 'exporting', action: () => ({ error: null }) }, EDIT: {} } },
    exporting: { on: { DONE: { target: 'choosing' }, FAIL: { target: 'failed', action: (_, e) => ({ error: e.error }) }, EDIT: {} } },
    failed: { on: { EXPORT: { target: 'exporting', action: () => ({ error: null }) }, EDIT: { target: 'choosing', action: () => ({ error: null }) } } },
  },
});

export function ExportDialog() {
  const records = useStore((s) => s.records);
  const [machine] = useState(createExport);
  const { value, context: { error } } = useMachine(machine);
  const [kind, setKind] = useState<ExportKind>('printer_bundle');
  const [picked, setPicked] = useState<Set<string>>(new Set());
  const list = kind === 'printer_bundle' || kind === 'printer' ? records.printers : kind === 'filament_bundle' || kind === 'filament' ? records.filaments : records.processes;
  const choices = list.filter((r) => !r.archived).map((r) => ({ id: r.id, name: r.name }));
  const selected = choices.filter((c) => picked.has(c.id));
  const edit = (next: () => void) => { next(); machine.send({ type: 'EDIT' }); };
  const run = async () => {
    if (!machine.can({ type: 'EXPORT' })) return;
    machine.send({ type: 'EXPORT' });
    try {
      const ids = selected.map((c) => c.id);
      if (kind === 'printer_bundle' || kind === 'filament_bundle') for (const id of ids) await exportProfiles({ kind, ids: [id] });
      else await exportProfiles({ kind, ids });
      machine.send({ type: 'DONE' });
      toast('exported_xx', { selectedCount: selected.length, value: kind.endsWith('bundle') ? 'bundle' : 'preset', value2: selected.length === 1 ? '' : 's' });
      close();
    } catch (e) { machine.send({ type: 'FAIL', error: e instanceof Error ? e.message : String(e) }); }
  };
  const busy = value === 'exporting';
  return (
    <Modal size="m" className="export-dialog" label="Export profiles" onClose={close}>
      <ModalTitle title="Export profiles" onClose={close} />
      <ModalBody>
        <label className="form-row"><span>Export as</span>
          <Dropdown className="combo" value={kind} disabled={busy} onValueChange={(k) => edit(() => { setKind(k as ExportKind); setPicked(new Set()); })} options={KINDS.map(([k, label]) => ({ value: k, label }))} /></label>
        {choices.length === 0 ? <p>You don't have any saved profiles of this type yet.</p> : <div className="modal-pick">
          <div className="modal-pick-head">
            <button type="button" className="link" disabled={busy} onClick={() => edit(() => setPicked(new Set(choices.map((c) => c.id))))}>Select all</button>
            <button type="button" className="link" disabled={busy} onClick={() => edit(() => setPicked(new Set()))}>Clear</button>
          </div>
          <div className="modal-pick-list" role="group" aria-label="Profiles to export">{choices.map((c) => (
            <label key={c.id} className="modal-check"><Check checked={picked.has(c.id)} disabled={busy}
              onCheckedChange={(checked) => edit(() => { const next = new Set(picked); if (checked) next.add(c.id); else next.delete(c.id); setPicked(next); })} /><span data-full-text={c.name}>{c.name}</span></label>
          ))}</div>
        </div>}
        {busy && <div role="status">Exporting…</div>}
        {error && <p className="modal-error" role="alert">{error}</p>}
      </ModalBody>
      <ModalFooter lead={choices.length > 0 && `${selected.length} of ${choices.length}`}>
        <button type="button" className="btn" onClick={close}>Cancel</button>
        <button type="button" className="btn-primary" data-output disabled={!selected.length || busy} onClick={() => void run()}>Export</button>
      </ModalFooter>
    </Modal>
  );
}
