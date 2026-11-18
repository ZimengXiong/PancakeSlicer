import { preferenceJson, savePreferences, usePreference } from '../../saved-preferences';
import { countRender } from '../../interaction-metrics';
import { Fragment, memo, useCallback, useEffect, useId, useLayoutEffect, useMemo, useRef, useState, type ComponentPropsWithRef, type ReactNode, type RefObject } from 'react';
import { flushSync } from 'react-dom';
import { Popover } from 'radix-ui';
import { useRowFill } from '../tab-rows';
import '../menu.css';
import './picker.css';
import { Caret } from '../Caret';
import { edgeOf, type Edge } from '../anchor';
import { SlotChip } from '../SlotChip';
import { joinParts } from '../../separator';
import { InLayer } from '../PageLayer';
export interface PickerItem { id: string; label: string; child?: boolean;  muted?: boolean; sub?: string; meta?: string; facet?: string; search?: string; disabled?: boolean | string;
  image?: string | null; status?: { label: string; tone: 'online' | 'printing' | 'paused' | 'offline' | 'error' }; slots?: { n: number; colour: string }[] }
export interface PickerGroup { id: string; title: string; items: PickerItem[]; filter?: boolean; allOnly?: boolean }
export interface PickerAction { label: string; run(): void; keep?: boolean }
interface Props { label: string; value?: string; groups(): PickerGroup[]; sections?: boolean; onPick(id: string): void; facetHeads?: boolean; memory?: string; footer?: PickerAction[]; className?: string; triggerProps?: ComponentPropsWithRef<'button'> & { 'data-autofocus'?: boolean | string }; empty?: string; children: ReactNode }
const read = (key: string): string[] => { const v = preferenceJson<unknown>(key, []); return Array.isArray(v) ? v.filter((s): s is string => typeof s === 'string') : []; };
const write = (key: string, value: string[]) => savePreferences({[key]:value});
const splitTail = (label: string): [string, string] => {
  for (const re of [/\s\S+\s\S+$/, /\s\S+$/]) { const m = re.exec(label); if (m && m[0].length <= 15 && m.index >= 4) return [label.slice(0, m.index), m[0]]; }
  return [label, ''];
};
interface RailTab { id: string; title: string; count?: number }
const AIM_DELAY = 80, AIM_WINDOW = 150;
function PickerRail({ tabs, active, hover, label, list, railRef, onChange }: { tabs: RailTab[]; active: string; hover: boolean; label: string; list: RefObject<HTMLElement | null>; railRef: RefObject<HTMLElement | null>; onChange(id: string): void }) {
  const timer = useRef<ReturnType<typeof setTimeout>>(undefined), trail = useRef<{ x: number; y: number; t: number }[]>([]), under = useRef('');
  useEffect(() => () => clearTimeout(timer.current), []);
  const note = (e: { clientX: number; clientY: number; timeStamp: number }) => { trail.current = [...trail.current.filter((p) => e.timeStamp - p.t < AIM_WINDOW), { x: e.clientX, y: e.clientY, t: e.timeStamp }]; };
  const aiming = () => {
    const now = performance.now(), pts = trail.current.filter((p) => now - p.t < AIM_WINDOW), box = list.current?.getBoundingClientRect(), from = pts[0], at = pts[pts.length - 1];
    if (!box || pts.length < 2 || now - at.t > AIM_DELAY || at.x <= from.x) return false;
    const slope = (a: { x: number; y: number }, b: { x: number; y: number }) => (b.y - a.y) / (b.x - a.x);
    const top = { x: box.left, y: box.top - 24 }, bottom = { x: box.left, y: box.bottom + 24 };
    return slope(at, top) < slope(from, top) && slope(at, bottom) > slope(from, bottom);
  };
  const intend = (id: string) => {
    clearTimeout(timer.current); under.current = id;
    const check = () => { if (under.current !== id) return; if (aiming()) timer.current = setTimeout(check, AIM_DELAY); else flushSync(() => onChange(id)); };
    check();
  };
  return <nav ref={railRef} className="pk-rail" aria-label={label} onPointerMove={hover ? (e) => { if (e.pointerType === 'mouse') note(e); } : undefined} onPointerLeave={() => { clearTimeout(timer.current); under.current = ''; }}>
    {tabs.map((r) => <button type="button" key={r.id} data-rail={r.id} className={active === r.id ? 'active' : ''} aria-pressed={active === r.id}
      onClick={() => { clearTimeout(timer.current); onChange(r.id); }}
      onPointerEnter={hover ? (e) => { if (e.pointerType !== 'mouse') return; note(e); if (r.id === active) { clearTimeout(timer.current); under.current = r.id; } else intend(r.id); } : undefined}>
      <span className="pk-rail-title">{r.title}</span>{r.count !== undefined && <span className="pk-count">{r.count}</span>}
    </button>)}
  </nav>;
}
const decoded = new Map<string, HTMLImageElement>();
const decodeAhead = (src: string) => { if (decoded.has(src)) return; const img = new Image(); img.src = src; decoded.set(src, img); img.decode().catch(() => {}); };
const idle = (run: () => void) => typeof requestIdleCallback === 'function' ? requestIdleCallback(run, { timeout: 3000 }) : setTimeout(run, 1500);
function PickerArt({ src }: { src: string | null }) {
  const [failed, setFailed] = useState<string>();
  return <span className="pk-art" aria-hidden>{src && failed !== src ? <img src={src} alt="" draggable={false} onError={() => setFailed(src)}/> : null}</span>;
}
const PickerRow = memo(function PickerRow({ row, index, id, active, selected, pinned, onActive, onPick, onPin }: {
  row: PickerItem; index: number; id: string; active: boolean; selected: boolean; pinned: boolean;
  onActive(index: number): void; onPick(row: PickerItem): void; onPin(row: PickerItem): void;
}) {
  countRender('picker-row');
  const media = row.image !== undefined, [head, tail] = splitTail(row.label);
  return <div className={`pk-row ${active ? 'is-active' : ''}`} data-index={index} data-child={row.child || undefined}>
    <button id={`${id}-${index}`} type="button" role="option" tabIndex={active ? 0 : -1} aria-selected={selected} aria-disabled={!!row.disabled} className={`pk-option${row.muted ? ' is-muted' : ''}`} onFocus={() => onActive(index)} onPointerMove={(e) => { if (e.pointerType === 'mouse') onActive(index); }} data-full-text={joinParts([row.label, row.sub, row.meta])} onClick={() => onPick(row)}>
      {media && <PickerArt src={row.image ?? null}/>}
      <span className="pk-text"><span className="pk-label"><span className="pk-head">{head}</span>{tail && <span className="pk-tail">{tail}</span>}</span>{row.sub && <span className="pk-sub">{row.sub}</span>}{media && row.meta && <span className="pk-sub">{row.meta}</span>}{row.status && <span className="pk-status status-text" data-tone={row.status.tone}>{row.status.label}</span>}</span>
      {!media && row.meta && <span className="pk-meta">{row.meta}</span>}
      {!!row.slots?.length && <span className="pk-slots" role="img" aria-label={`Also in slot ${row.slots.map(c => c.n).join(', ')}`}>{row.slots.map(c => <SlotChip key={c.n} n={c.n} colour={c.colour}/>)}</span>}
    </button>
    <button type="button" className="pk-star" aria-label={`Favorite ${row.label}`} aria-pressed={pinned} onClick={() => onPin(row)}>{pinned ? '★' : '☆'}</button>
  </div>;
});
type TabRows = { tab: string; rows: PickerItem[]; heads: Map<number, string> };
const PickerTab = memo(function PickerTab({ t, on, active, id, value, pinned, empty, onActive, onPick, onPin }: {
  t: TabRows; on: boolean; active: number; id: string; value?: string; pinned: Set<string>; empty: string;
  onActive(index: number): void; onPick(row: PickerItem): void; onPin(row: PickerItem): void;
}) {
  return <div className="pk-tab" data-off={on ? undefined : ''}>{t.rows.map((row, i) => <Fragment key={`${i}:${row.id}`}>{t.heads.has(i) && <div className="pk-group" role="presentation">{t.heads.get(i)}</div>}<PickerRow row={row} index={i} id={`${id}-${t.tab}`} active={on && active === i} selected={value === row.id} pinned={pinned.has(row.id)} onActive={onActive} onPick={onPick} onPin={onPin} /></Fragment>)}{!t.rows.length && <div className="pk-empty">{empty}</div>}</div>;
});
export function Picker({ label, value, groups, sections = false, onPick, facetHeads = false, memory = `orca.parity.${label}`, footer = [], className = '', triggerProps, empty = 'No profiles', children }: Props) {
  countRender('picker');
  const triggerEl = useRef<HTMLButtonElement | null>(null), [edge, setEdge] = useState<Edge>({ align: 'start' });
  const [open, setOpen] = useState(false), [primed, setPrimed] = useState(false), [rail, setRail] = useState(''), [active, setActive] = useState(0);
  const savedPins = usePreference(`${memory}.pins`);
  const pins = useMemo(() => read(`${memory}.pins`), [memory,savedPins]);
  const railRef = useRef<HTMLElement>(null), body = useRef<HTMLDivElement>(null), list = useRef<HTMLDivElement>(null), typed = useRef({text: '', at: 0});
  const id = useId();
  const live = open || primed;
  const pool = useMemo(() => live ? groups().map(g => ({...g, items:g.items.filter(item => !item.disabled)})).filter((g) => g.items.length) : [], [live, groups]);
  const pinnedSet = useMemo(() => new Set(pins), [pins]);
  const tabbed = useMemo(() => pool.filter(g => !g.allOnly), [pool]);
  const rails = useMemo<RailTab[]>(() => sections ? [{id: 'all', title: 'All'}, ...(tabbed.every(g => !g.title) ? [] : tabbed.map(g => ({id: g.id, title: g.title})))] : [...(pool.some(g => g.items.some(r => pinnedSet.has(r.id))) ? [{id:'pins',title:'Favorites'}] : []), {id:'all',title:'All profiles'}, ...pool.map(({id,title}) => ({id,title}))], [sections, pinnedSet, pool, tabbed]);
  useRowFill(railRef, open && rails.map(r => r.id).join('|'));
  const activeRail = rails.some(r => r.id === rail) ? rail : 'all';
  const all = useMemo(() => [...new Map(pool.filter(g => !g.filter).flatMap((g) => g.items).map((item) => [item.id, item])).values()], [pool]);
  const tabs = useMemo<TabRows[]>(() => rails.map(r => {
    if (!sections) return {tab: r.id, rows: r.id === 'pins' ? all.filter(x => pinnedSet.has(x.id)) : r.id === 'all' ? all : pool.find(g => g.id === r.id)?.items ?? [], heads: new Map<number, string>()};
    const g = r.id === 'all' ? undefined : tabbed.find(t => t.id === r.id), favourites = (g ? g.items : all).filter(x => pinnedSet.has(x.id));
    const rest = (items: PickerItem[]) => items.filter(x => !pinnedSet.has(x.id));
    const byFacet = (items: PickerItem[]) => { const at = new Map<string, PickerItem[]>(); for (const x of items) at.set(x.facet ?? '', [...at.get(x.facet ?? '') ?? [], x.sub === x.facet ? {...x, sub: undefined} : x]); return [...at].map(([title, rows]) => ({title, items: rows})); };
    const secs = [{title: 'Favorites', items: favourites}, ...(g ? (facetHeads ? byFacet(rest(g.items)) : [{title: g.title, items: rest(g.items)}])
      : [...pool.filter(x => !x.filter && !x.allOnly), ...pool.filter(x => x.allOnly)].map(x => ({title: x.title, items: rest(x.items)})))].filter(x => x.items.length);
    const at = new Map<number, string>(); let i = 0;
    if (!g || facetHeads || secs.length > 1) for (const sec of secs) { at.set(i, sec.title); i += sec.items.length; }
    return {tab: r.id, rows: secs.flatMap(sec => sec.items), heads: at};
  }), [rails, sections, facetHeads, tabbed, pinnedSet, all, pool]);
  const rows = useMemo(() => tabs.find(t => t.tab === activeRail)?.rows ?? [], [tabs, activeRail]);
  const fill = useRef(0);
  useLayoutEffect(() => {
    const box = body.current, l = list.current;
    if (!box || !l) return;
    if (!open) { fill.current = 0; box.style.removeProperty('--pk-fill'); return; }
    const pad = parseFloat(getComputedStyle(l).paddingTop) + parseFloat(getComputedStyle(l).paddingBottom);
    const need = Math.max(...[...l.children].map(t => t.scrollHeight + pad), railRef.current?.scrollHeight ?? 0);
    if (need > fill.current) { fill.current = need; box.style.setProperty('--pk-fill', `${need}px`); }
    const pop = box.parentElement, foot = pop?.querySelector<HTMLElement>(':scope > .pk-foot');
    if (pop) box.style.setProperty('--pk-chrome', `${(foot?.offsetHeight ?? 0) + pop.offsetHeight - pop.clientHeight}px`);
  }, [open, tabs]);
  const groupsRef = useRef(groups); groupsRef.current = groups;
  const warm = useCallback(() => { for (const g of groupsRef.current()) for (const item of g.items) if (item.image) decodeAhead(item.image); }, []);
  useEffect(() => { const handle = idle(warm); return () => { if (typeof cancelIdleCallback === 'function' && typeof handle === 'number') cancelIdleCallback(handle); else clearTimeout(handle as ReturnType<typeof setTimeout>); }; }, [warm]);
  const enabled = useMemo(() => rows.flatMap((r, i) => r.disabled ? [] : [i]), [rows]);
  const resetType = () => { typed.current = {text: '', at: 0}; };
  const move = (index: number) => {
    if (index < 0 || !rows[index] || rows[index].disabled) return;
    setActive(index);
    const el = list.current?.querySelector<HTMLButtonElement>(`.pk-tab:not([data-off]) [data-index="${index}"] .pk-option`);
    el?.focus({preventScroll: true}); el?.scrollIntoView({ block: 'nearest' });
  };
  const focusChoice = () => { const selected = rows.findIndex(r => r.id === value && !r.disabled); move(selected >= 0 ? selected : enabled[0] ?? -1); };
  const focusOnOpen = useRef(focusChoice); focusOnOpen.current = focusChoice;
  useEffect(() => { if (open && list.current) focusOnOpen.current(); }, [open]);
  const prime = () => { warm(); setPrimed(true); };
  const unprime = () => { if (!open) setPrimed(false); };
  const changeRail = (next: string) => { setRail(next); setActive(0); resetType(); if (list.current) list.current.scrollTop = 0; };
  const railTab = (tab: string) => railRef.current?.querySelector<HTMLButtonElement>(`[data-rail="${CSS.escape(tab)}"]`);
  const pick = useCallback((row: PickerItem) => {
    if (row.disabled) return;
    onPick(row.id);
    setOpen(false); resetType();
  }, [onPick]);
  const pin = useCallback((row: PickerItem) => { const next = pins.includes(row.id) ? pins.filter((id) => id !== row.id) : [...pins, row.id]; write(`${memory}.pins`, next); }, [pins, memory]);
  return <Popover.Root open={open} onOpenChange={next => { setOpen(next); resetType(); if (!next) setPrimed(false); if (next) { setEdge(edgeOf(triggerEl.current)); setRail(''); setActive(0); } }}>
    <Popover.Trigger asChild><button {...triggerProps} ref={(el) => { triggerEl.current = el; const r = triggerProps?.ref; if (typeof r === 'function') r(el); else if (r) r.current = el; }} type="button" className={`pk-trigger dropdown ${className}`} data-menu-trigger="orca" aria-label={label} onPointerEnter={prime} onFocus={warm} onPointerLeave={unprime} onKeyDown={e => {
      triggerProps?.onKeyDown?.(e);
      if (!e.defaultPrevented && (e.key === 'ArrowDown' || e.key === 'ArrowUp')) { e.preventDefault(); e.stopPropagation(); resetType(); setRail(''); setOpen(true); }
    }}>{children}<Caret className="pk-caret"/></button></Popover.Trigger>
    <InLayer portal={Popover.Portal} forceMount={primed || undefined}><Popover.Content forceMount={primed || undefined} className={`orca-menu pk-pop ${rows.some(r => r.image !== undefined) ? 'pk-media' : ''}`} data-menu-family="orca" aria-label={label} side="bottom" align={edge.align} sideOffset={4} collisionPadding={8} onOpenAutoFocus={e => { e.preventDefault(); if (open) focusChoice(); }} onKeyDown={e => {
      if (e.key === 'Escape' || e.key === 'Tab') return;
      e.stopPropagation();
      const at = enabled.indexOf(active), inRail = e.target instanceof Element && !!e.target.closest('.pk-rail');
      if (rails.length && inRail && (e.key === 'ArrowDown' || e.key === 'ArrowUp')) {
        e.preventDefault(); const i = rails.findIndex(r => r.id === activeRail), next = rails[(i + (e.key === 'ArrowDown' ? 1 : -1) + rails.length) % rails.length].id;
        changeRail(next); requestAnimationFrame(() => railTab(next)?.focus());
      }
      else if (e.key === 'ArrowDown' || e.key === 'ArrowUp') { e.preventDefault(); resetType(); if (enabled.length) move(enabled[(at + (e.key === 'ArrowDown' ? 1 : -1) + enabled.length) % enabled.length]); }
      else if (e.key === 'Home' || e.key === 'End') { e.preventDefault(); resetType(); move(enabled[e.key === 'Home' ? 0 : enabled.length - 1] ?? -1); }
      else if (e.key === 'Enter' && e.target instanceof Element && e.target.closest('.pk-option') && rows[active]) { e.preventDefault(); e.altKey ? pin(rows[active]) : pick(rows[active]); }
      else if (rails.length && (e.key === 'ArrowLeft' || e.key === 'ArrowRight')) {
        e.preventDefault();
        if (e.key === 'ArrowRight' && inRail) move(enabled[0] ?? -1);
        else if (e.key === 'ArrowLeft' && !inRail) railTab(activeRail)?.focus();
      }
      else if (e.key.length === 1 && e.key !== ' ' && !e.ctrlKey && !e.metaKey && !e.altKey) {
        e.preventDefault();
        const now = Date.now(), old = now - typed.current.at < 700 ? typed.current.text : '';
        const key = e.key.toLocaleLowerCase(), term = old === key ? key : old + key;
        typed.current = {text: term, at: now};
        const ordered = [...enabled.filter(i => i > active), ...enabled.filter(i => i <= active)];
        const match = ordered.find(i => `${rows[i].label} ${rows[i].sub ?? ''} ${rows[i].search ?? ''}`.toLocaleLowerCase().split(/\s+/).some(word => word.startsWith(term)) || rows[i].label.toLocaleLowerCase().replace(/^my\s+/, '').startsWith(term));
        if (match !== undefined) move(match);
      }
    }}>
      <div ref={body} className="pk-body">
        <PickerRail tabs={rails} active={activeRail} hover label={`${label} groups`} list={list} railRef={railRef} onChange={changeRail} />
        <div className="pk-list" role="listbox" aria-label={label} ref={list}>{tabs.map(t => <PickerTab key={t.tab} t={t} on={t.tab === activeRail} active={t.tab === activeRail ? active : -1} id={id} value={value} pinned={pinnedSet} empty={empty} onActive={setActive} onPick={pick} onPin={pin} />)}</div>
      </div>
      <div className="pk-foot">{footer.map(a => <button type="button" className="btn" key={a.label} onClick={() => { if (!a.keep) setOpen(false); a.run(); }}>{a.label}</button>)}</div>
    </Popover.Content></InLayer>
  </Popover.Root>;
}
