import { observePreference, preferenceJson, savePreferences } from '../saved-preferences';
import { useEffect, useLayoutEffect, useMemo, useRef, useState, useSyncExternalStore } from 'react';
import { getState, setState, useStore } from '../state/store';
import { requestPreset } from '../components/PresetCombo';
import { Keys } from '../components/Keys';
import { Modal } from '../components/Modal';
import { allCommands, allProviders, closePalette, commandsVersion, paletteQuery, registerProvider, subscribeCommands, subscribePalette, type Command } from './registry';
import { registerCore } from './core';
import { fuzzy, loadSettings, settingCommands } from './settings';
import { SEP } from '../separator';
import { slotFilaments } from '../profiles/slots';

registerCore();

const RECENT_KEY = 'orca.web.palette.recent';
let recent: string[] = [];
observePreference(RECENT_KEY, () => { const value = preferenceJson<unknown>(RECENT_KEY, []); recent = Array.isArray(value) ? value.filter((v):v is string=>typeof v === 'string') : []; });
function remember(id: string) {
  recent = [id, ...recent.filter((r) => r !== id)].slice(0, 20);
  savePreferences({[RECENT_KEY]:recent});
}

registerProvider('presets', (q) => {
  if (!q) return [];
  const s = getState();
  const row = (type: 'machine' | 'process' | 'filament', group: string, name: string, current: string, slot = 0): Command => ({
    id: `preset.${type}.${slot}.${name}`, title: name, group, detail: name === current ? 'Current' : undefined,
    keywords: type === 'machine' ? 'printer machine' : type, run: () => requestPreset(type, slot, name),
  });
  const lists = { machine: s.records.printers, process: s.records.processes, filament: s.records.filaments } as const;
  const users = (t: string) => (lists[t as keyof typeof lists] ?? []).filter((u) => !u.archived).map((u) => u.name);
  return [
    ...users('machine').map((n) => row('machine', 'Printer', n, s.printer)),
    ...users('process').map((n) => row('process', 'Process', n, s.process)),
    ...slotFilaments(s).flatMap((cur, slot) => users('filament')
      .map((n) => row('filament', slotFilaments(s).length > 1 ? `Filament ${slot + 1}` : 'Filament', n, cur, slot))),
  ];
});

const objectCommands = (): Command[] => getState().objects.map((o) => ({
  id: `object.${o.id}`, title: o.name, group: 'Object on plate', keywords: 'select zoom',
  detail: o.instances.length > 1 ? `×${o.instances.length}` : undefined,
  run: () => {
    setState({ tab: 'prepare', selected: { object: o.id, instance: o.instances[0].id, allInstances: true } });
    requestAnimationFrame(() => window.dispatchEvent(new Event('camera-fit')));
  },
}));

let sessions: Command[] = [];
const ago = (t: number) => {
  const m = Math.round((Date.now() - t) / 60000);
  return m < 1 ? 'now' : m < 60 ? `${m} min ago` : m < 1440 ? `${Math.round(m / 60)} h ago` : `${Math.round(m / 1440)} d ago`;
};
function loadSessions() {
  void import('../flow/project').then((m) => {
    sessions = (m.projectList() ?? []).map((x) => ({
      id: `recent.${x.id}`, title: x.name, group: 'Recent project', keywords: 'open restore session',
      detail: `${x.objects} object${x.objects === 1 ? '' : 's'}${SEP}${ago(x.savedAt)}`, run: () => void m.openProject(x.id),
    }));
  }).catch(() => {});
}

interface Row { c: Command; score: number; hits: number[]; off: string | false }

const enabledState = (c: Command): string | false => {
  const e = c.enabled?.();
  return e === undefined || e === true ? false : typeof e === 'string' ? e : 'Not available';
};

function rank(list: Command[], q: string): Row[] {
  const out: Row[] = [];
  for (const c of list) {
    if (!c.run) continue;
    const t = fuzzy(q, c.title);
    const k = c.keywords ? fuzzy(q, `${c.group} ${c.keywords}`) : fuzzy(q, c.group);
    if (!t && !k) continue;
    const at = c.title.toLowerCase().indexOf(q.toLowerCase());
    const base = Math.max(t ? t.score + (at === 0 ? 60 : at > 0 ? 30 : 0) : 0, k ? k.score * 0.6 : 0);
    const r = recent.indexOf(c.id);
    const off = enabledState(c);
    out.push({ c, hits: t && t.score >= (k?.score ?? 0) * 0.6 ? t.hits : [], off, score: base + (c.boost ?? 0) + (r >= 0 ? 60 - 2 * r : 0) - (off ? 120 : 0) });
  }
  return out.sort((a, b) => b.score - a.score);
}

function suggestions(): [string, Row[]][] {
  const s = getState();
  const byId = new Map(allCommands().map((c) => [c.id, c]));
  const row = (c: Command): Row => ({ c, score: 0, hits: [], off: enabledState(c) });
  const recentRows = recent.map((id) => byId.get(id)).filter((c): c is Command => !!c?.run && !enabledState(c)).slice(0, 5).map(row);
  const next = (!s.objects.length ? ['file.import', 'file.projects', 'file.new'] : s.selected
    ? ['tool.flatten', 'tool.rotate', 'tool.move', 'obj.orient', 'obj.add', 'edit.delete']
    : ['file.send', 'file.export', 'obj.arrange', 'obj.orient', 'view.preview'])
    .map((id) => byId.get(id)).filter((c): c is Command => !!c && !recentRows.some((r) => r.c === c) && !enabledState(c)).map(row);
  return [['Recent', recentRows], ['Suggested', next], ['On the plate', objectCommands().slice(0, 6).map(row)]];
}

const SCOPES: Record<string, string> = { '>': 'Commands', '/': 'Settings', '@': 'Objects' };

function Marked({ text, hits }: { text: string; hits: number[] }) {
  const all = new Set(hits);
  const set = new Set(hits.filter((h) => all.has(h - 1) || all.has(h + 1)));
  if (!set.size) return <>{text}</>;
  return <>{[...text].map((c, i) => set.has(i) ? <b key={i}>{c}</b> : c)}</>;
}

export default function Palette() {
  const initial = useSyncExternalStore(subscribePalette, paletteQuery) ?? '';
  useSyncExternalStore(subscribeCommands, commandsVersion);
  useStore((s) => s.objects);
  const [query, setQuery] = useState(initial);
  const [active, setActive] = useState(0);
  const [async, setAsync] = useState<{ q: string; rows: Command[] }>({ q: '', rows: [] });
  const list = useRef<HTMLUListElement>(null);
  useEffect(() => { loadSessions(); void loadSettings(); }, []);
  useEffect(() => { setQuery(initial); setActive(0); }, [initial]);

  const scope = SCOPES[query[0]] ? query[0] : '';
  const q = (scope ? query.slice(1) : query).trim();

  useEffect(() => {
    if (!q || scope === '>' || scope === '@') return;
    let live = true;
    const asks = [...(scope === '/' ? [] : allProviders().map(([, p]) => p(q))), settingCommands(q)];
    void Promise.all(asks.map((a) => Promise.resolve(a).catch(() => []))).then((r) => live && setAsync({ q: query, rows: r.flat() }));
    return () => { live = false; };
  }, [query]);

  const sections: [string, Row[]][] = useMemo(() => {
    if (!q && !scope) return suggestions().filter(([, rows]) => rows.length);
    if (!q) return [[SCOPES[scope], scope === '@' ? objectCommands().map((c) => ({ c, score: 0, hits: [], off: false })) : scope === '>' ? allCommands().filter((c) => c.run).map((c) => ({ c, score: 0, hits: [], off: enabledState(c) })) : []]];
    const late = async.q === query ? async.rows : [];
    if (scope === '/') return [['Settings', late.filter((c) => c.group === 'Setting').sort((a, b) => (b.boost ?? 0) - (a.boost ?? 0)).map((c) => ({ c, score: 0, hits: fuzzy(q, c.title)?.hits ?? [], off: false }))]];
    if (scope === '@') return [['Objects', rank(objectCommands(), q)]];
    const cmds = rank([...allCommands(), ...(scope ? [] : [...objectCommands(), ...sessions])], q);
    if (scope === '>') return [['Commands', cmds]];
    const settings = late.filter((c) => c.group === 'Setting').sort((a, b) => (b.boost ?? 0) - (a.boost ?? 0)).slice(0, 5)
      .map((c) => ({ c, score: 0, hits: fuzzy(q, c.title)?.hits ?? [], off: false as const }));
    const presets = rank(late.filter((c) => c.group !== 'Setting'), q).slice(0, 8);
    return [['', [...cmds, ...presets].sort((a, b) => b.score - a.score).slice(0, 40)], ['Settings', settings]];
  }, [q, scope, async, query, initial]);
  const rows = sections.flatMap(([, r]) => r);
  const at = Math.min(active, Math.max(0, rows.length - 1));

  useLayoutEffect(() => { list.current?.querySelector('.pal-row.active')?.scrollIntoView({ block: 'nearest' }); }, [at, rows.length]);

  const run = (r: Row | undefined) => {
    if (!r || r.off) return;
    remember(r.c.id);
    closePalette();
    setTimeout(() => r.c.run!(), 0);
  };
  let i = -1;
  return (
    <Modal size="l" className="palette" label="Command palette" onClose={closePalette} backdropClose>
      <div className="pal-input-row">
        {scope && <span className="pal-scope">{SCOPES[scope]}</span>}
        <input className="pal-input" data-autofocus autoFocus spellCheck={false} autoComplete="off"
          placeholder={scope === '/' ? 'Search settings' : scope === '@' ? 'Objects on the plate' : 'Search commands, settings, profiles, or objects…'}
          aria-label="Command" role="combobox" aria-expanded={rows.length > 0} aria-controls="pal-list" aria-activedescendant={rows.length ? `pal-${at}` : undefined}
          value={scope ? query.slice(1) : query}
          onChange={(e) => { setQuery(scope + e.target.value); setActive(0); }}
          onKeyDown={(e) => {
            if (e.key === 'ArrowDown' || (e.key === 'n' && e.ctrlKey)) setActive((a) => (a + 1) % Math.max(1, rows.length));
            else if (e.key === 'ArrowUp' || (e.key === 'p' && e.ctrlKey)) setActive((a) => (a - 1 + rows.length) % Math.max(1, rows.length));
            else if (e.key === 'PageDown' || e.key === 'PageUp') setActive((a) => Math.max(0, Math.min(rows.length - 1, a + (e.key === 'PageDown' ? 8 : -8))));
            else if (e.key === 'Enter') run(rows[at]);
            else if (e.key === 'Backspace' && scope && !e.currentTarget.value) setQuery('');
            else return;
            e.preventDefault();
          }} />
      </div>
      <ul className="pal-list" id="pal-list" role="listbox" aria-label="Commands" ref={list}>
        {sections.map(([title, rs]) => rs.length > 0 && [
          ...rs.map((r) => {
            const n = ++i;
            return (
              <li key={`${title}:${r.c.id}`} id={`pal-${n}`} role="option" aria-selected={n === at} aria-disabled={!!r.off || undefined}
                className={`pal-row ${n === at ? 'active' : ''} ${r.off ? 'off' : ''}`} title={r.off || undefined}
                onMouseMove={() => n !== active && setActive(n)} onClick={() => run(r)}>
                <span className="pal-title"><Marked text={r.c.title} hits={r.hits} /></span>
                {(r.off || r.c.detail) && <span className="pal-detail">{r.off || r.c.detail}</span>}
                {r.c.keys && <Keys keys={r.c.keys} className="pal-keys" />}
              </li>
            );
          }),
        ])}
        {q && !rows.length && <li className="pal-empty" role="status">{scope === '/' && async.q !== query ? 'Loading settings…' : `No match for “${q}”`}</li>}
      </ul>
    </Modal>
  );
}
