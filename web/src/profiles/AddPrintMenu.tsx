import { useId, useMemo, useRef, useState, type ReactNode, type RefObject } from 'react';
import { Popover } from 'radix-ui';
import type { ProcessRecord } from './records';
import { getState, useStore } from '../state/store';
import { Dropdown } from '../components/Dropdown';
import { Glyph } from '../components/Glyph';
import { addPrintMaster, createPrintProfile, freeName, isAdded, layerRange, usePrintMasters, type PrintMaster } from './print-catalog';
import { newProfileNameError } from './save-to-profile';
import '../components/menu.css';
import './add-print-menu.css';
import { InLayer } from '../components/PageLayer';

const message = (e: unknown) => e instanceof Error ? e.message : String(e);
const copyName = (base: string) => freeName(getState(), `${base.replace(/ copy( \d+)?$/, '')} copy`);

export function AddPrintMenu({ open, onOpenChange, anchor, onOpen, onCreated, children }: {
  open: boolean; onOpenChange(open: boolean): void; anchor?: RefObject<HTMLElement | null>; onOpen(key: string): void; onCreated(key: string): void; children: ReactNode;
}) {
  return <Popover.Root open={open} onOpenChange={onOpenChange}>
    {anchor ? <><Popover.Anchor virtualRef={anchor as RefObject<HTMLElement>}/>{children}</> : <Popover.Trigger asChild>{children}</Popover.Trigger>}
    <InLayer portal={Popover.Portal}><Popover.Content className="orca-menu add-print" data-menu-family="orca" aria-label="Add print settings" side="bottom" align="start" sideOffset={4} collisionPadding={8}
      onOpenAutoFocus={(e) => { e.preventDefault(); (e.currentTarget as HTMLElement | null)?.querySelector<HTMLElement>('[data-autofocus]')?.focus(); }}
      onKeyDown={(e) => { if (e.key !== 'Escape' && e.key !== 'Tab') e.stopPropagation(); }}>
      {open && <AddPrintBody close={() => onOpenChange(false)} onOpen={onOpen} onCreated={onCreated}/>}
    </Popover.Content></InLayer>
  </Popover.Root>;
}

function AddPrintBody({ close, onOpen, onCreated }: { close(): void; onOpen(key: string): void; onCreated(key: string): void }) {
  const users = useStore((s) => s.records.processes); useStore((s) => s.printer);
  const { data, error } = usePrintMasters(true);
  const [query, setQuery] = useState(''), [active, setActive] = useState(0), [busy, setBusy] = useState<string | null>(null), [failed, setFailed] = useState<string | null>(null);
  const [naming, setNaming] = useState(false);
  const list = useRef<HTMLDivElement>(null), id = useId();
  const s = getState();
  const shown = useMemo(() => { const q = query.trim().toLocaleLowerCase(), all = data ?? [], named = all.filter((m) => m.name.toLocaleLowerCase().includes(q));
    return q ? [...named, ...all.filter((m) => !named.includes(m) && m.description.toLocaleLowerCase().includes(q))] : all; }, [data, query]);
  const rows = shown.length + 1;
  if (naming) return <NewProfileForm masters={data ?? []} users={users} back={() => setNaming(false)} onCreated={(key) => { close(); onCreated(key); }}/>;
  const activate = async (i: number) => {
    const m = shown[i];
    if (!m) { setNaming(true); return; }
    if (isAdded(getState(), m)) { close(); onOpen(m.id); return; }
    if (busy) return;
    setBusy(m.id); setFailed(null);
    try { await addPrintMaster(m); } catch (e) { setFailed(message(e)); } finally { setBusy(null); }
  };
  const move = (i: number) => { const next = (i + rows) % rows; setActive(next); list.current?.querySelector(`[data-index="${next}"]`)?.scrollIntoView({ block: 'nearest' }); };
  const row = (m: PrintMaster, i: number) => {
    const added = isAdded(s, m);
    return <button key={m.id} id={`${id}-${i}`} type="button" role="option" tabIndex={-1} data-index={i} aria-selected={active === i} className="menu-item add-print-row" data-added={added || undefined}
      onPointerMove={() => setActive(i)} onClick={() => void activate(i)}>
      <span className="add-print-text"><span className="add-print-name">{m.name}</span><span className="add-print-line">{m.description}</span></span>
      <span className="add-print-side"><span className="add-print-state">{busy === m.id ? 'Adding…' : added ? <><span aria-hidden>✓</span>Added</> : 'Add'}</span><span className="add-print-line">{layerRange(s, m)}</span></span>
    </button>;
  };
  return <div className="add-print-body" onKeyDown={(e) => {
    if (e.key === 'ArrowDown' || e.key === 'ArrowUp') { e.preventDefault(); move(active + (e.key === 'ArrowDown' ? 1 : -1)); }
    else if (e.key === 'Enter') { e.preventDefault(); void activate(active); }
  }}>
    <input className="text add-print-search" type="search" data-autofocus aria-label="Search print profiles" placeholder="Search" value={query}
      role="combobox" aria-expanded aria-controls={`${id}-list`} aria-activedescendant={`${id}-${active}`} onChange={(e) => { setQuery(e.target.value); setActive(0); }}/>
    <div id={`${id}-list`} ref={list} className="add-print-list" role="listbox" aria-label="Print profiles">
      {shown.map(row)}
      {!data && <div className="add-print-empty" role="status">{error ?? 'Loading…'}</div>}
      {data && !shown.length && <div className="add-print-empty">No profiles match</div>}
      <div className="menu-sep" role="presentation"/>
      <button id={`${id}-${shown.length}`} type="button" role="option" tabIndex={-1} data-index={shown.length} aria-selected={active === shown.length} className="menu-item add-print-new" onPointerMove={() => setActive(shown.length)} onClick={() => setNaming(true)}>New profile…</button>
    </div>
    {failed && <div className="add-print-error" role="alert">{failed}</div>}
  </div>;
}

function NewProfileForm({ masters, users, back, onCreated }: { masters: PrintMaster[]; users: ProcessRecord[]; back(): void; onCreated(key: string): void }) {
  const s = getState();
  const owned = users.filter((p) => !p.archived);
  const bases = [...owned.map((p) => ({ value: p.id, label: p.name })), ...masters.filter((m) => !isAdded(s, m)).map((m) => ({ value: m.id, label: m.name }))];
  const current = owned.find((p) => p.id === s.process || p.name === s.process);
  const [base, setBase] = useState(current ? current.id : bases[0]?.value ?? '');
  const baseName = bases.find((b) => b.value === base)?.label ?? '';
  const [name, setName] = useState(() => copyName(baseName)), [edited, setEdited] = useState(false);
  const [busy, setBusy] = useState(false), [failed, setFailed] = useState<string | null>(null);
  const nameError = useStore((st) => newProfileNameError(st, 'process', name)), errorId = useId();
  const create = async () => {
    if (busy || nameError || !base) return;
    const profile = owned.find((p) => p.id === base), master = masters.find((m) => m.id === base);
    setBusy(true); setFailed(null);
    try { onCreated(await createPrintProfile(profile ? { profile } : { master: master! }, name)); }
    catch (e) { setFailed(message(e)); setBusy(false); }
  };
  return <form className="add-print-form" onSubmit={(e) => { e.preventDefault(); void create(); }}>
    <div className="add-print-form-head"><button type="button" className="icon-btn" aria-label="Back to print profiles" onClick={back}><Glyph name="back"/></button><span>New profile</span></div>
    <label className="add-print-field"><span>Based on</span>
      <Dropdown aria-label="Based on" value={base} options={bases} onValueChange={(v) => { setBase(v); if (!edited) setName(copyName(bases.find((b) => b.value === v)?.label ?? '')); }}/></label>
    <label className="add-print-field"><span>Name</span>
      <input className="text" data-autofocus autoFocus value={name} aria-invalid={!!nameError} aria-describedby={nameError ? errorId : undefined} onChange={(e) => { setName(e.target.value); setEdited(true); }} onFocus={(e) => e.currentTarget.select()}/></label>
    {nameError && name.trim() && <p className="control-error" id={errorId} role="status">{nameError}</p>}
    {failed && <div className="add-print-error" role="alert">{failed}</div>}
    <div className="add-print-actions"><button type="button" className="btn" onClick={back}>Cancel</button><button type="submit" className="btn primary" disabled={busy || !!nameError || !base}>{busy ? 'Creating…' : 'Create'}</button></div>
  </form>;
}
