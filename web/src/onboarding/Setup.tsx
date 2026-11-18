import { useEffect, useMemo, useState, type CSSProperties, type ReactNode } from 'react';
import { requestPreset } from '../components/PresetCombo';
import { Glyph } from '../components/Glyph';
import { getState, setState, toast, useStore, type State } from '../state/store';
import { recordOf } from '../profiles/view';
import type { PrinterRecord } from '../profiles/records';
import { savePreferences } from '../saved-preferences';
import { addCatalogPrinters, addCatalogProducts, useCatalog } from '../profiles/catalog-add';
import type { CatalogEntry } from '../catalog/types';
import { FIRST_SCREEN, loadedPrinterVendors, printerCatalog, type CatalogModel, type CatalogVendor } from '../profiles/catalog-reads';
import { addPrintMaster, printMasters, usePrintMasters } from '../profiles/print-catalog';
import { discoverPrinters, saveConnection, testConnection, type ConnectionEdit, type ConnectionKind, type ConnectionTest, type Discovered } from '../printers/connections';
import { setup, TOUR_PAGES, useSetup } from './setup-flow';
import { TOUR } from './tour';
import { FacetGroup, FacetsHead } from '../components/Facets';
import { Check } from '../components/Check';
import { Dropdown } from '../components/Dropdown';
import { materialRank } from '../profiles/Pickers';
import './setup.css';
import { Sep } from '../separator';

const send = setup.send;
const VENDOR_NAMES: Record<string, string> = { BBL: 'Bambu Lab' };
const vendorName = (id: string, name = id) => VENDOR_NAMES[id] ?? name;

interface PrinterModel { id: string; vendor: string; vendorName: string; model: string; cover?: string; nozzles: number[]; catalog: CatalogModel }
interface Choices { model: PrinterModel | null; nozzles: number[]; profiles: Set<string> | null; connection: ConnectionEdit | null; filaments: Set<string> | null; products: CatalogEntry[] }
let choices: Choices = { model: null, nozzles: [], profiles: null, connection: null, filaments: null, products: [] };

const toModels = (vendors: CatalogVendor[]): PrinterModel[] => vendors.flatMap((v) => v.models.map((m) => ({ id: `${v.id}|${m.id}`, vendor: v.id,
  vendorName: vendorName(v.id, v.name), model: m.id, cover: m.cover, nozzles: m.nozzles.map(Number).filter((n) => n > 0), catalog: m })));
const selectedPrinter = (s: State) => { const r = recordOf(s, 'machine', s.printer) as PrinterRecord | undefined; return r && !r.archived ? r : undefined; };

const STEPS = ['printer', 'profiles', 'connect', 'filaments'] as const;
function Frame({ step, children, foot, title }: { step: string; children: ReactNode; foot?: ReactNode; title?: string }) {
  const at = STEPS.indexOf(step as (typeof STEPS)[number]);
  return (
    <div className="setup" role="dialog" aria-modal="true" aria-label="Setup">
      <div className={`setup-panel${step === 'welcome' ? ' setup-panel-welcome' : ''}`}>
        <header className="setup-head">
          <img src="/logo.svg" width={24} height={24} alt="" />
          {title && <h1 className="setup-title">{title}</h1>}
          {at >= 0 && <div className="setup-dots" aria-label={`Step ${at + 1} of ${STEPS.length}`}>{STEPS.map((s, i) => <span key={s} className={i <= at ? 'on' : ''} />)}</div>}
          <button type="button" className="icon-btn setup-close" aria-label="Close setup" onClick={() => send({ type: 'CLOSE' })}><Glyph name="close" /></button>
        </header>
        <div className="setup-body">{children}</div>
        {foot && <footer className="setup-foot">{foot}</footer>}
      </div>
    </div>
  );
}
const Back = () => <button type="button" className="btn-ghost" onClick={() => send({ type: 'BACK' })}>Back</button>;
const Next = ({ disabled, label = 'Next', onClick }: { disabled?: boolean; label?: string; onClick?(): void }) =>
  <button type="button" className="btn-primary" disabled={disabled} onClick={onClick ?? (() => send({ type: 'NEXT' }))}>{label}</button>;

function Welcome() {
  const ready = useStore((s) => !!selectedPrinter(s));
  const idle = ready && !TOUR_PAGES;
  useEffect(() => { if (idle) send({ type: 'CLOSE' }); }, [idle]);
  if (idle) return null;
  return ready
    ? <Frame step="welcome" title="Want a quick 1-minute tour?">
      <div className="setup-welcome">
        <p>Take a quick look at what makes PancakeSlicer special: smooth curved tops, dynamic profiles, and projects that remember every version!</p>
        <div className="setup-welcome-actions"><button type="button" className="btn-ghost" onClick={() => send({ type: 'CLOSE' })}>Not now</button><Next label="Start tour" /></div>
      </div>
    </Frame>
    : <Frame step="welcome" title="Welcome to PancakeSlicer!">
      <div className="setup-welcome">
        <p>Let's pick your printer and filaments—it only takes about a minute!</p>
        <div className="setup-welcome-actions">{TOUR_PAGES > 0 && <button type="button" className="btn-ghost" onClick={() => send({ type: 'NEXT' })}>Show me around</button>}<Next label="Set up printer" onClick={() => send({ type: 'SKIP' })} /></div>
      </div>
    </Frame>;
}
function Shot({ src, caption }: { src: string; caption: string }) {
  const [ratio, setRatio] = useState<number>();
  return <figure className="setup-shot">
    <div className="setup-shot-frame"><img src={src} alt={caption} style={ratio ? { '--ar': ratio } as CSSProperties : undefined}
      onLoad={(e) => setRatio(e.currentTarget.naturalWidth / e.currentTarget.naturalHeight)} /></div>
    <figcaption>{caption}</figcaption>
  </figure>;
}
function Tour({ page }: { page: number }) {
  const t = TOUR[page];
  if (!t) return null;
  return <Frame step="tour" foot={<><Back /><button type="button" className="btn-ghost" onClick={() => send({ type: 'SKIP' })}>Skip</button><span className="setup-grow" />
    <span className="setup-count">{page + 1}/{TOUR_PAGES}</span><Next /></>}>
    <div className="setup-tour">
      <div className="setup-tour-art">{t.shots.map(([src, caption]) => <Shot key={src} src={src} caption={caption} />)}</div>
      <div className="setup-tour-text"><h2>{t.title}</h2>{t.points.map((x) => <p key={x}>{x}</p>)}</div>
    </div>
  </Frame>;
}

function Printer() {
  const [models, setModels] = useState<PrinterModel[] | null>(() => { const c = loadedPrinterVendors(); return c ? toModels(c) : null; });
  const [failed, setFailed] = useState<string | null>(null);
  useEffect(() => { if (!models) void printerCatalog().then((v) => setModels(toModels(v)), (e) => setFailed(e instanceof Error ? e.message : String(e))); }, []);
  const vendors = useMemo(() => [...new Map((models ?? []).map((m) => [m.vendor, m.vendorName])).entries()].sort((a, b) => a[1].localeCompare(b[1])), [models]);
  const [vendor, setVendor] = useState<string | null>(choices.model?.vendor ?? 'BBL');
  const [search, setSearch] = useState('');
  const [model, setModel] = useState<PrinterModel | null>(choices.model);
  const [nozzles, setNozzles] = useState<number[]>(choices.nozzles);
  const q = search.trim().toLowerCase();
  const shown = (models ?? []).filter((m) => q ? `${m.vendorName} ${m.model}`.toLowerCase().includes(q) : m.vendor === vendor);
  const pick = (m: PrinterModel) => { setModel(m); setNozzles(m.nozzles.includes(0.4) ? [0.4] : m.nozzles.slice(0, 1)); };
  useEffect(() => {
    if (model || !models) return;
    const current = selectedPrinter(getState());
    const found = current && models.find((m) => m.vendor === current.vendor && m.catalog.id === current.model);
    if (!found) return;
    const nozzle = current.nozzles[0]?.diameter ?? 0;
    setVendor(found.vendor); setModel(found); setNozzles(found.nozzles.includes(nozzle) ? [nozzle] : found.nozzles.includes(0.4) ? [0.4] : found.nozzles.slice(0, 1));
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [models]);
  const toggle = (n: number) => setNozzles((list) => list.includes(n) ? list.filter((x) => x !== n) : [...list, n].sort((a, b) => a - b));
  const next = () => { if (model) { choices = { ...choices, model, nozzles }; send({ type: 'NEXT' }); } };
  return <Frame step="printer" foot={<><Back /><span className="setup-grow" />{failed && <span className="setup-test bad">{failed}</span>}
    {model && <span className="setup-count">{model.model}<Sep/>{nozzles.join(', ')} mm</span>}<Next disabled={!model || !nozzles.length} onClick={next} /></>}>
    <h2>Your printer</h2>
    <input className="text setup-search" type="search" placeholder="Search printers" aria-label="Search printers" value={search} onChange={(e) => setSearch(e.target.value)} />
    <div className="setup-printer">
      {!q && <div className="setup-brands" role="tablist" aria-label="Brand">{vendors.map(([id, name]) =>
        <button key={id} type="button" role="tab" aria-selected={id === vendor} className="setup-brand" onClick={() => setVendor(id)}>{name}</button>)}</div>}
      <div className="setup-models">{!models && !failed && <span className="setup-count">Loading printers…</span>}{shown.map((m, i) => (
        <button key={m.id} type="button" className="setup-model" aria-pressed={model?.id === m.id} onClick={() => pick(m)}>
          {m.cover ? <img src={m.cover} alt="" loading={i < FIRST_SCREEN ? 'eager' : 'lazy'} onError={(e) => { (e.currentTarget as HTMLImageElement).style.visibility = 'hidden'; }} /> : <span className="setup-model-blank" />}
          <span>{q ? `${m.vendorName} ` : ''}{m.model}</span>
        </button>
      ))}</div>
    </div>
    {model && model.nozzles.length > 0 && <div className="setup-row"><span className="setup-k">Nozzle</span><div className="setup-chips" role="group" aria-label="Nozzle">{model.nozzles.map((n) =>
      <label key={n} className="modal-check"><Check checked={nozzles.includes(n)} onCheckedChange={() => toggle(n)} />{n} mm</label>)}</div></div>}
  </Frame>;
}

function Profiles() {
  const { data, error } = usePrintMasters(true);
  const rows = data ?? [];
  const [on, setOn] = useState<Set<string> | null>(choices.profiles);
  const picked = on ?? new Set(rows.map((m) => m.id));
  const flip = (k: string) => setOn(() => { const n = new Set(picked); if (n.has(k)) n.delete(k); else n.add(k); return n; });
  const next = () => { choices = { ...choices, profiles: picked }; send({ type: 'NEXT' }); };
  return <Frame step="profiles" foot={<><Back /><span className="setup-grow" />{error && <span className="setup-test bad">{error}</span>}<span className="setup-count">{picked.size} selected</span><Next disabled={!picked.size} onClick={next} /></>}>
    <h2>Print profiles</h2>
    <div className="setup-list">{!data && !error && <span className="setup-count">Loading print profiles…</span>}{rows.map((m) => (
      <label key={m.id} className="setup-check"><input type="checkbox" checked={picked.has(m.id)} onChange={() => flip(m.id)} /><span>{m.name}</span></label>))}</div>
  </Frame>;
}

const KINDS: [ConnectionKind, string][] = [['bambu-lan', 'Bambu LAN'], ['moonraker', 'Moonraker'], ['octoprint', 'OctoPrint'], ['flashforge', 'Flashforge']];
function Connect() {
  const [edit, setEdit] = useState<ConnectionEdit>(choices.connection ?? { kind: 'bambu-lan', name: 'Printer' });
  const [test, setTest] = useState<ConnectionTest | 'testing' | null>(null);
  const [found, setFound] = useState<Discovered[] | 'looking' | string | null>(null);
  const discover = async () => {
    setFound('looking');
    try { const r = await discoverPrinters(); setFound(r.printers.length ? r.printers : 'We didn\'t spot any printers on your network yet'); }
    catch (e) { setFound(e instanceof Error ? e.message : String(e)); }
  };
  const use = (d: Discovered) => { setEdit((e) => ({ ...e, kind: d.kind, name: d.name ?? e.name, host: d.host, url: d.url, serial: d.serial, model: d.model })); setTest(null); };
  const set = (patch: Partial<ConnectionEdit>) => { setEdit((e) => ({ ...e, ...patch })); setTest(null); };
  const field = (key: keyof ConnectionEdit, label: string, type = 'text') => <label className="setup-field"><span className="setup-k">{label}</span>
    <input className="text" type={type} value={String(edit[key] ?? '')} onChange={(e) => set({ [key]: e.target.value })} /></label>;
  const runTest = async () => { setTest('testing'); try { setTest(await testConnection('', edit)); } catch (e) { setTest({ ok: false, stage: 'error', message: e instanceof Error ? e.message : String(e) }); } };
  const filled = edit.kind === 'bambu-lan' ? !!(edit.host && edit.serial && edit.access_code) : edit.kind === 'flashforge' ? !!edit.host : !!edit.url;
  const next = () => { choices = { ...choices, connection: filled ? edit : null }; send({ type: 'NEXT' }); };
  return <Frame step="connect" foot={<><Back /><button type="button" className="btn-ghost" onClick={() => { choices = { ...choices, connection: null }; send({ type: 'SKIP' }); }}>Skip</button><span className="setup-grow" /><Next disabled={!filled} onClick={next} /></>}>
    <h2>Connect your printer</h2>
    <div className="setup-row">
      <button type="button" className="btn" disabled={found === 'looking'} onClick={() => void discover()}>{found === 'looking' ? 'Looking…' : 'Find printers'}</button>
      {typeof found === 'string' && found !== 'looking' && <span className="setup-count">{found}</span>}
    </div>
    {Array.isArray(found) && <div className="setup-found">{found.map((d, i) => (
      <button key={`${d.host ?? d.url}-${i}`} type="button" className="setup-chip" aria-pressed={!!d.host && d.host === edit.host || !!d.url && d.url === edit.url} onClick={() => use(d)}>
        {d.name ?? d.model ?? d.host ?? d.url} <span className="setup-n">{d.host ?? d.url}</span></button>))}</div>}
    <label className="setup-field setup-kind"><span className="setup-k">Connection</span><Dropdown className="combo" aria-label="Connection" value={edit.kind} onValueChange={(next) => set({ kind: next as ConnectionKind })} options={KINDS.map(([k, l]) => ({ value: k, label: l }))} /></label>
    <div className="setup-form">
      {edit.kind === 'bambu-lan' ? <>{field('host', 'IP address')}{field('serial', 'Serial number')}{field('access_code', 'Access code', 'password')}</>
        : edit.kind === 'flashforge' ? <>{field('host', 'IP address')}{field('serial', 'Serial number (optional)')}{field('access_code', 'Check code (optional)', 'password')}</>
        : <>{field('url', 'Address')}{edit.kind === 'octoprint' && field('key', 'API key', 'password')}</>}
    </div>
    <div className="setup-row">
      <button type="button" className="btn" disabled={!filled || test === 'testing'} onClick={() => void runTest()}>{test === 'testing' ? 'Testing…' : 'Test'}</button>
      {test && test !== 'testing' && <span className={`setup-test ${test.ok ? 'ok' : 'bad'}`} role="status">{test.ok ? <>Connected{test.model && <><Sep/>{test.model}</>}</> : test.message}</span>}
    </div>
  </Frame>;
}

interface FilamentRow { key: string; name: string; brand: string; material: string; product: CatalogEntry }
const baseMaterial = (m: string) => { const b = m.toUpperCase().split(/[-+ ]/)[0].replace(/\d+$/, ''); return b || m; };
function Filaments({ error }: { error: string | null }) {
  const model = choices.model;
  const { data, error: readError } = useCatalog('filament', !!model);
  const rows = useMemo<FilamentRow[]>(() => (data && 'products' in data ? data.products : [])
    .map((p) => ({ key: p.id, name: p.id, brand: p.brand || 'Generic', material: p.type || 'Other', product: p }))
    .sort((a, b) => a.name.localeCompare(b.name)), [data]);
  const home = model ? model.vendorName.split(' ')[0] : '';
  const counted = (values: string[]) => [...values.reduce((m, v) => m.set(v, (m.get(v) ?? 0) + 1), new Map<string, number>())].map(([value, count]) => ({ value, label: value, count }));
  const brandOptions = useMemo(() => counted(rows.map((r) => r.brand)).sort((a, b) => a.label.localeCompare(b.label)), [rows]);
  const materialOptions = useMemo(() => counted(rows.map((r) => baseMaterial(r.material))).sort((a, b) => materialRank(a.value) - materialRank(b.value) || a.value.localeCompare(b.value)), [rows]);
  const [brands, setBrands] = useState<string[]>([]);
  const [materials, setMaterials] = useState<string[]>([]);
  const [search, setSearch] = useState('');
  const [filtersOpen, setFiltersOpen] = useState(false);
  const [on, setOn] = useState<Set<string> | null>(choices.filaments);
  useEffect(() => {
    if (!rows.length || on) return;
    const b = brandOptions.find((o) => home && o.value.startsWith(home));
    if (b) setBrands([b.value]);
    setOn(new Set(['PLA', 'PETG'].map((m) => {
      const of = (brandRows: FilamentRow[]) => brandRows.filter((r) => r.material === m).sort((x, y) => Number(/basic/i.test(y.name)) - Number(/basic/i.test(x.name)) || x.name.length - y.name.length)[0];
      return (home && of(rows.filter((r) => r.brand.startsWith(home)))) || of(rows.filter((r) => /^generic\b/i.test(r.name)));
    }).filter((r): r is FilamentRow => !!r).map((r) => r.key)));
  }, [rows, on, brandOptions, home]);
  const chosen = on ?? new Set<string>();
  const q = search.trim().toLowerCase();
  const shown = rows.filter((r) => (!brands.length || brands.includes(r.brand)) && (!materials.length || materials.includes(baseMaterial(r.material)))
    && (!q || `${r.brand} ${r.name} ${r.material}`.toLowerCase().includes(q)));
  const picked = shown.filter((r) => chosen.has(r.key)).length;
  const setMany = (keys: string[], value: boolean) => setOn((s) => { const n = new Set(s); keys.forEach((k) => value ? n.add(k) : n.delete(k)); return n; });
  const apply = () => { choices = { ...choices, filaments: chosen, products: rows.filter((r) => chosen.has(r.key))
    .sort((a, b) => materialRank(baseMaterial(a.material)) - materialRank(baseMaterial(b.material))).map((r) => r.product) }; send({ type: 'APPLY' }); void applyChoices(); };
  const active = brands.length + materials.length;
  const item = (r: FilamentRow) => <label key={r.key} className="setup-check"><Check checked={chosen.has(r.key)} onCheckedChange={(v) => setMany([r.key], v)} /><span>{r.name}</span><span className="setup-n">{r.material}</span></label>;
  return <Frame step="filaments" foot={<><Back /><span className="setup-grow" />{(error ?? readError) && <span className="setup-test bad">{error ?? readError}</span>}<span className="setup-count">{chosen.size} selected</span><Next disabled={!chosen.size} onClick={apply} /></>}>
    <h2>Your filaments</h2>
    <div className={`setup-filaments${filtersOpen ? ' filters-open' : ''}`}>
      <aside className="facets setup-facets" aria-label="Filters">
        <FacetsHead active={active} onClear={() => { setBrands([]); setMaterials([]); }} />
        <FacetGroup label="Brand" options={brandOptions} value={brands} onChange={setBrands} />
        <FacetGroup label="Material" options={materialOptions} value={materials} onChange={setMaterials} />
        <button type="button" className="btn setup-show" onClick={() => { setFiltersOpen(false); requestAnimationFrame(() => document.querySelector<HTMLElement>('.setup-filters-btn')?.focus()); }}>Show {shown.length}</button>
      </aside>
      <section className="setup-results" aria-label="Filaments">
        <div className="setup-results-tools">
          <button type="button" className="btn setup-filters-btn" onClick={() => setFiltersOpen(true)}><Glyph name="filter" />{active ? `Filters (${active})` : 'Filters'}</button>
          <input className="text setup-search" type="search" placeholder="Search filaments" aria-label="Search filaments" value={search} onChange={(e) => setSearch(e.target.value)} />
        </div>
        <label className="setup-check setup-all"><Check checked={picked === 0 ? false : picked === shown.length ? true : 'indeterminate'} disabled={!shown.length}
          onCheckedChange={() => setMany(shown.map((r) => r.key), picked < shown.length)} /><span>Select all shown</span><span className="setup-n">{shown.length}</span></label>
        <div className="setup-grid">{shown.map(item)}
          {!shown.length && <span className="setup-count">{data ? 'No filaments match' : 'Loading filaments…'}</span>}</div>
      </section>
    </div>
  </Frame>;
}

async function applyChoices() {
  try {
    const { model, nozzles, profiles, connection } = choices;
    const sizes = model ? nozzles.map((n) => model.catalog.nozzles.find((k) => Number(k) === n)).filter((k): k is string => !!k) : [];
    const printers = model && sizes.length ? await addCatalogPrinters(sizes.map((nozzle) => ({ vendor: model.vendor, model: model.catalog, nozzle }))) : undefined;
    const printerKey = printers?.chosen[0] ?? null;
    if (printerKey) requestPreset('machine', 0, printerKey);
    if (profiles?.size) {
      let first: string | undefined;
      for (const m of (await printMasters()).filter((m) => profiles.has(m.id))) { const key = await addPrintMaster(m); first ??= key; }
      if (first) requestPreset('process', 0, first);
    }
    if (connection && printerKey) await saveConnection(printerKey, { ...connection, name: model?.model ?? 'Printer' }).catch((e) => toast('couldnt_save_connection', { detail: e instanceof Error ? e.message : e }));
    const added = choices.products.length ? await addCatalogProducts(choices.products.map((product) => ({ product }))) : undefined;
    const first = added?.chosen[0];
    if (first) requestPreset('filament', 0, first);
    send({ type: 'APPLIED' });
  } catch (e) { send({ type: 'FAILED', error: e instanceof Error ? e.message : String(e) }); }
}

function Done() {
  const finish = () => { savePreferences({ 'orca.setup': '1' }); setState({ tab: 'prepare' }); send({ type: 'CLOSE' }); };
  return <Frame step="done" foot={<><Back /><span className="setup-grow" /><Next label="Start printing" onClick={finish} /></>}>
    <div className="setup-hero"><img src="/logo.svg" width={96} height={96} alt="" /><h1>Ready</h1></div>
  </Frame>;
}

export default function Setup() {
  const { value, context } = useSetup();
  useEffect(() => {
    if (value === 'closed') return;
    const before = document.activeElement as HTMLElement | null;
    const panel = () => document.querySelector<HTMLElement>('.setup');
    const behind = [...(panel()?.parentElement?.children ?? [])].filter((el): el is HTMLElement => el instanceof HTMLElement && !el.classList.contains('setup') && !el.inert);
    behind.forEach((el) => { el.inert = true; });
    const trap = (e: KeyboardEvent) => {
      const root = panel();
      if (e.key !== 'Tab' || !root) return;
      const all = [...root.querySelectorAll<HTMLElement>('button:not(:disabled),input:not(:disabled),[tabindex]:not([tabindex="-1"]),a[href]')].filter((el) => el.offsetParent !== null);
      if (!all.length) return;
      const firstEl = all[0], lastEl = all[all.length - 1], at = document.activeElement;
      if (!root.contains(at)) { e.preventDefault(); firstEl.focus(); }
      else if (e.shiftKey && at === firstEl) { e.preventDefault(); lastEl.focus(); }
      else if (!e.shiftKey && at === lastEl) { e.preventDefault(); firstEl.focus(); }
    };
    document.addEventListener('keydown', trap, true);
    return () => { document.removeEventListener('keydown', trap, true); behind.forEach((el) => { el.inert = false; }); before?.focus?.(); };
  }, [value === 'closed']);
  useEffect(() => {
    if (value === 'closed') return;
    const root = document.querySelector<HTMLElement>('.setup');
    const visible = (el: HTMLElement) => el.offsetParent !== null;
    ([...(root?.querySelectorAll<HTMLElement>('.setup-body input, .setup-body button') ?? [])].find(visible) ?? root?.querySelector<HTMLElement>('.setup-foot .btn-primary'))?.focus();
  }, [value, context.page]);
  useEffect(() => {
    if (value === 'closed') return;
    const onKey = (e: KeyboardEvent) => { if (e.key === 'Escape' && value !== 'applying') send({ type: 'CLOSE' }); };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [value]);
  useEffect(() => { if (value === 'closed') choices = { model: null, nozzles: [], profiles: null, connection: null, filaments: null, products: [] }; }, [value]);
  switch (value) {
    case 'closed': return null;
    case 'welcome': return <Welcome />;
    case 'tour': return <Tour page={context.page} />;
    case 'printer': return <Printer />;
    case 'profiles': return <Profiles />;
    case 'connect': return <Connect />;
    case 'filaments': return <Filaments error={context.error} />;
    case 'applying': return <Frame step="filaments" foot={<span className="setup-count">Setting up…</span>}><div className="setup-hero"><span className="setup-spin" /></div></Frame>;
    case 'done': return <Done />;
  }
}
