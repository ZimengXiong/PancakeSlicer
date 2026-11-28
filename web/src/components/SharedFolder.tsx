import { useEffect, useState, useSyncExternalStore } from 'react';
import { Modal, ModalBody, ModalFooter, ModalTitle } from './Modal';
import { promptDialog } from './Ask';
import { mutation, responseError } from '../api/access';
import { toast } from '../state/store';

export interface ShareEntry { name: string; dir?: boolean; size: number; modified: number }
export interface SharePick { paths: string[]; name: string; exists: boolean }
type Browse = { title: string; action: string; extensions: string[]; multiple?: boolean; save?: string; resolve(v: SharePick | null): void };

let current: Browse | null = null;
const listeners = new Set<() => void>();
const show = (b: Browse | null) => {
  current = b;
  listeners.forEach((l) => l());
};

export const browseShare = (b: Omit<Browse, 'resolve'>) =>
  new Promise<SharePick | null>((resolve) => {
    current?.resolve(null);
    show({ ...b, resolve });
  });

const join = (dir: string, name: string) => (dir ? `${dir}/${name}` : name);
const size = (n: number) => n < 1e6 ? `${Math.max(1, Math.round(n / 1e3))} KB` : `${(n / 1e6).toFixed(1)} MB`;
const folderProblem = (v: string) => !v.replace(/[ .]/g, '') ? 'Enter a name.'
  : /[/\\]/.test(v) || v.startsWith('.') ? 'Names can\'t contain slashes or start with a dot.'
  : ['@eaDir', '#recycle', '#snapshot'].includes(v) ? 'That name is reserved.'
  : /\p{Cc}/u.test(v) ? 'Names can\'t contain control characters.'
  : new TextEncoder().encode(v).length > 120 ? 'That name is too long.' : null;

function SharedFolderDialog({ b }: { b: Browse }) {
  const [dir, setDir] = useState('');
  const [entries, setEntries] = useState<ShareEntry[] | null>(null);
  const [error, setError] = useState('');
  const [picked, setPicked] = useState<string[]>([]);
  const [name, setName] = useState(b.save ?? '');
  useEffect(() => {
    let live = true;
    setEntries(null); setError(''); setPicked([]);
    fetch(`/api/share?path=${encodeURIComponent(dir)}`)
      .then(async (r) => { if (!r.ok) throw await responseError(r); return r.json() as Promise<{ entries: ShareEntry[] }>; })
      .then((v) => { if (live) setEntries(v.entries); }, (e) => { if (live) setError(e instanceof Error ? e.message : String(e)); });
    return () => { live = false; };
  }, [dir]);
  const done = (v: SharePick | null) => {
    show(null);
    b.resolve(v);
  };
  const fits = (n: string) => b.extensions.some((x) => n.toLowerCase().endsWith(x));
  const files = (entries ?? []).filter((e) => e.dir || fits(e.name));
  const ext = b.extensions[0];
  const saveName = name.trim() && !fits(name.trim()) ? `${name.trim()}${ext}` : name.trim();
  const problem = b.save === undefined ? null : !saveName.slice(0, -ext.length).trim() ? 'Enter a name.'
    : /[/\\]/.test(saveName) || saveName.startsWith('.') ? 'Names can\'t contain slashes or start with a dot.' : null;
  const submit = () => {
    if (b.save !== undefined) {
      if (!problem && entries) done({ paths: [join(dir, saveName)], name: saveName, exists: entries.some((e) => !e.dir && e.name === saveName) });
    } else if (picked.length) done({ paths: picked.map((p) => join(dir, p)), name: picked[0], exists: true });
  };
  const choose = (e: ShareEntry) => {
    if (e.dir) return setDir(join(dir, e.name));
    if (b.save !== undefined) return setName(e.name);
    setPicked(b.multiple ? (picked.includes(e.name) ? picked.filter((p) => p !== e.name) : [...picked, e.name]) : [e.name]);
  };
  const parts = dir ? dir.split('/') : [];
  const newFolder = async () => {
    const folder = await promptDialog('New folder', 'New folder', folderProblem, 'Create');
    if (!folder) return;
    try {
      await mutation(`/api/share/folder?path=${encodeURIComponent(join(dir, folder))}`, { method: 'POST' }, false, undefined, undefined, true);
      setDir(join(dir, folder));
    } catch (e) {
      toast('couldnt_create_folder', { detail: e instanceof Error ? e.message : String(e) });
    }
  };
  return (
    <Modal size="m" label={b.title} onClose={() => done(null)}>
      <form method="dialog" onSubmit={(e) => { e.preventDefault(); submit(); }}>
        <ModalTitle title={b.title} onClose={() => done(null)} />
        <ModalBody>
          <div className="share-bar">
            <nav className="share-crumbs" aria-label="Folder">
              {['Shared folder', ...parts].map((p, i) => i === parts.length
                ? <span key={i} aria-current="page">{p}</span>
                : <span key={i}><button type="button" className="link" onClick={() => setDir(parts.slice(0, i).join('/'))}>{p}</button> /</span>)}
            </nav>
            <button type="button" className="btn" disabled={!entries} onClick={() => void newFolder()}>New folder</button>
          </div>
          <div className="share-list" role="listbox" aria-label={b.title} aria-multiselectable={b.multiple || undefined}>
            {error ? <p className="modal-error" role="alert">{error}</p>
              : !entries ? <p className="share-empty">Loading…</p>
              : !files.length ? <p className="share-empty">This folder has no {b.extensions.map((x) => x.slice(1).toUpperCase()).join(', ')} files.</p>
              : files.map((e) => (
                <button key={e.name} type="button" role="option" aria-selected={picked.includes(e.name) || b.save !== undefined && e.name === saveName}
                  className="share-row" onClick={() => choose(e)}
                  onDoubleClick={() => { if (!e.dir && b.save === undefined) done({ paths: [join(dir, e.name)], name: e.name, exists: true }); }}>
                  <span className="share-name">{e.dir ? `${e.name}/` : e.name}</span>
                  {!e.dir && <span className="share-meta">{size(e.size)} · {new Date(e.modified).toLocaleString()}</span>}
                </button>
              ))}
          </div>
          {b.save !== undefined && <label className="form-row"><span>File name</span>
            <input className="text" aria-label="File name" aria-invalid={!!problem || undefined} value={name} onChange={(e) => setName(e.target.value)} data-autofocus /></label>}
          {problem && name && <p className="ask-problem" role="alert">{problem}</p>}
        </ModalBody>
        <ModalFooter lead={b.save === undefined && b.multiple ? `${picked.length} selected` : undefined}>
          <button type="button" className="btn" onClick={() => done(null)}>Cancel</button>
          <button type="submit" className="btn-primary" disabled={b.save !== undefined ? !!problem || !entries : !picked.length}>{b.action}</button>
        </ModalFooter>
      </form>
    </Modal>
  );
}

export function SharedFolderHost() {
  const b = useSyncExternalStore((l) => { listeners.add(l); return () => void listeners.delete(l); }, () => current);
  return b ? <SharedFolderDialog key={b.title} b={b} /> : null;
}
