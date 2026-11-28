import { Skeleton } from '../components/Skeleton';
import { savePreferences, usePreference } from '../saved-preferences';
import { ProfileLibraryNotice } from '../components/ProfileLibraryNotice';
import { RemovedElsewhere } from './RemovedElsewhere';
import { useEffect, useId, useRef } from 'react';
import { createSelection, reduceSelection, selectedVisible, type SelectionEvent } from './selection';
import { findProfile, matchesProfile, profileKey } from './identity';
import { FacetGroup, FacetsHead } from '../components/Facets';
import { Caret } from '../components/Caret';
import { ManagerProfileRow } from './ManagerProfileRow';
import { recordProblems } from '../persist/profile-library';
import { copyRecord, creationPending, restoreProfile } from './transactions';
import { newProfileNameError } from './save-to-profile';
import { documentGeneration } from '../state/history';
import { DetailPane, ProfileEditor, type ProfileDraft } from './ProfileEditor';
import { projectDeltaKey, sharedDraft, updateSharedDraft } from './shared-draft';
import { addCatalogPrinter, addCatalogPrinters, addCatalogProduct, addCatalogProducts, useCatalog, type CatalogRead } from './catalog-add';
import type { CatalogModel } from './catalog-reads';
import type { CatalogEntry } from '../catalog/types';
import { profileInfo, useAsks, type ProfileBase, type ProfileRecord } from './view';
import { firmwareName, loadedLine } from '../catalog/read';
import { materialRank } from './Pickers';
import { modelLabel, nozzleField, rowDetail, vendorField, vendorLabel } from './metadata';
import { Check } from '../components/Check';
import { requestPreset } from '../components/PresetCombo';
import { ActionMenu, MenuButton } from '../components/Menu';
import { Segmented } from '../components/kit';
import { Dropdown } from '../components/Dropdown';
import './library-page.css';
import { useContainerTier } from '../breakpoints';
import { Glyph } from '../components/Glyph';
import { useCallback, useMemo, useState } from 'react';
import type { PrinterRecord, ProcessRecord } from './records';
import type { Config, ConfigValue, PresetType } from '../api/types';
import { accessSnapshot } from '../api/access';
import { OPTIONS } from '../options';
import { getState, resolvePreset, setState, toast, useStore, type State } from '../state/store';
import { useMachine } from '../flow/fsm';
import { libraryWrites } from './library-writes';
import { windowOps, type CopyRequest, type WindowOp } from '../flow/profiles-window-ops';
import { SettingsDetail } from '../components/VisibilityControl';
import { AddPrintMenu } from './AddPrintMenu';
import { confirmDialog, promptDialog } from '../components/Ask';
import { diffRows, useTabLoaded } from '../components/PresetDialogs';
import { display } from './format';
import { changeCount, takeValue } from './stack';
import { SEP } from '../separator';
import { filamentPlacements, placementText, useOnPrinter } from './on-printer';
import { slotFilaments } from './slots';
import { exportProfiles } from './library-io';
import { removeFromLibrary, undoRemoval } from '../flow/library-removal';

const TYPES: [PresetType, string][] = [['machine', 'Printers'], ['filament', 'Filaments'], ['process', 'Print settings']];
const first = (value: unknown) => String(Array.isArray(value) ? value[0] ?? '' : value ?? '');
const coverOf = (vendor: string, model: string) => vendor && model ? `/profiles/${encodeURIComponent(vendor)}/${encodeURIComponent(model)}_cover.png` : undefined;

interface Item { base?:ProfileBase; key: string; name: string; mine: boolean; changed: number; slot?: number; record?: ProfileRecord; note: string; vendor: string; model:string; nozzle: string; material: string; family?: string; cover?: string|null;reason?: string; problem?: string; meta?: string; fullName?: string; child?: boolean; config: Config; variants?: Item[]; catalog?: CatalogModel | CatalogEntry; loaded?: string; remaining?: number | null; pinned?: boolean }
const COMMON_MATERIALS = ['PLA', 'PETG', 'ABS', 'ASA', 'TPU'];
const NOUNS: Record<PresetType,[string,string]> = {machine:['printer','printers'],filament:['filament','filaments'],process:['print setting','print settings']};
const count = (n:number,[one,many]:[string,string]) => `${n} ${n === 1 ? one : many}`;
interface Group { key: string; label: string; sub?: string; rows: Item[]; children?: Group[] }
const isDynamic = (r?: ProfileRecord) => !!(r as ProcessRecord | undefined)?.scales?.length || !!(r as ProcessRecord | undefined)?.speedModel;
const groupHas = (g: Group, match: (i: Item) => boolean): boolean => g.children ? g.children.some((c) => groupHas(c, match)) : g.rows.some(match);
const byLabel = (a: string, b: string) => a === 'Other' || a === 'Any printer' ? 1 : b === 'Other' || b === 'Any printer' ? -1 : parseFloat(a) - parseFloat(b) || a.localeCompare(b);
const sessionFolds = new Map<string, boolean>();
const unique = (rows: Item[]) => [...new Map(rows.map(r => [r.key, r])).values()];
const slotsOf = (s: State, type: PresetType, name: string) => {
  const profile = findProfile(s, type, name), matches = (key: string) => profile ? matchesProfile(profile, key) : key === name;
  return type === 'machine' ? (matches(s.printer) ? [0] : []) : type === 'process' ? (matches(s.process) ? [0] : []) : slotFilaments(s).flatMap((f,i) => matches(f) ? [i] : []);
};


function items(s: State, type: PresetType, removed = false): Item[] {
  const list = (type === 'machine' ? s.records.printers : type === 'process' ? s.records.processes : s.records.filaments) as ProfileRecord[];
  const item = (record: ProfileRecord): Item => {
    const info = profileInfo(s, type, record.id), slots = slotsOf(s, type, record.id), resolved = resolvePreset(s, type, record.id);
    const printer = type === 'machine' ? record as PrinterRecord : undefined, nozzle = printer ? String(printer.nozzles[0]?.diameter ?? '') : '';
    const vendor = info.vendor ?? '', model = info.model ?? '', material = info.entry?.type ?? '';
    const note = type === 'machine' ? [model, nozzle && `${nozzle} mm nozzle`].filter(Boolean).join(SEP) : type === 'filament' ? [material, vendor].filter(Boolean).join(SEP) : '';
    return { key: record.id, name: record.name, mine: true, record, base: resolved.base, config: resolved.config, vendor, model, nozzle, material,
      cover: type === 'machine' ? coverOf(vendor, model) ?? null : undefined, slot: slots[0], note, changed: slots.reduce((n, i) => n + changeCount(s, type, i), 0),
      problem: recordProblems(s, type, record.id, record.name).join('; ') || undefined };
  };
  const inUse = (i: Item) => (i.slot === undefined ? 1 : 0);
  const order = (a: Item, b: Item) => inUse(a) - inUse(b) || (a.name < b.name ? -1 : 1);
  if (removed) return list.filter((r) => r.archived).map((r) => ({ ...item(r), note: `Removed ${new Date(r.archived!.at).toLocaleDateString([], { month: 'short', day: 'numeric' })}` }))
    .sort((a, b) => (b.record?.archived?.at ?? 0) - (a.record?.archived?.at ?? 0));
  return list.filter((r) => !r.archived).map(item).sort(order);
}

const failed = (action: string, e: unknown) => {
  const message = e instanceof Error ? e.message : String(e);
  if (accessSnapshot()?.message !== message) toast('failed', { action, message });
};

async function archiveRecords(type: PresetType, keys: string[]) {
  const s=getState(), rows=keys.flatMap(key=>findProfile(s,type,key) ?? []);
  if(!rows.length)throw new Error('Pick a custom profile that you can delete.');
  const archived=new Set(rows.map(profileKey)),linked=s.printerConnections.connections.filter(c=>archived.has(c.printer));
  if(type === 'machine' && s.records.printers.every(p=>p.archived || archived.has(profileKey(p))))throw new Error(rows.length === 1 ? 'You\'ll need at least one printer! Add another one before removing this one.' : 'You\'ll need at least one printer — leave one of these unchecked!');
  const devices=linked.length ? `\nThe connection to ${linked.map(c=>[c.model || c.name,c.host].filter(Boolean).join(' at ')).join(', ')} is removed too; reconnect it from another printer profile.` : '';
  const placements=type==='filament' ? rows.map(p=>[p.name,placementText(filamentPlacements(s,profileKey(p)))] as const).filter(([,where])=>where) : [];
  const used=placements.length ? `\n${placements.map(([name,where])=>`${rows.length > 1 ? name+': ' : ''}Used by ${where}`).join('\n')}\nThose slots go back to "Pick a filament…".` : '';
  const current=type === 'machine' ? rows.find(p=>matchesProfile(p,s.printer)) : type === 'process' ? rows.find(p=>matchesProfile(p,s.process)) : undefined;
  const project=current ? `\nThis project uses ${current.name}; it waits for you to choose another ${type === 'machine' ? 'printer' : 'print profile'}.` : '';
  if(!await confirmDialog('Remove from library',`Say goodbye to ${rows.length === 1 ? rows[0].name : `${rows.length} profiles`} from your library?\n${rows.length > 1 ? rows.map(p=>p.name).join('\n')+'\n' : ''}Don't worry, your saved projects will keep their settings, and you can always bring it back from Removed in the Library filters.${project}${used}${devices}`,['Remove','Cancel']))return;
  
  const undo=await removeFromLibrary(type,rows.map(profileKey));
  toast('removed_profilex_library', { rowsCount: rows.length, value: rows.length === 1 ? '' : 's' }, { action: {label:'Undo',run:()=>void undoRemoval(undo).then(()=>toast('profiles_restored')).catch(error=>failed('Undo',error))} });
}
export async function removePreset(type: PresetType, key: string) {try {await archiveRecords(type,[key]);} catch(error) {failed('Remove',error);}}

function catalogRows(data: CatalogRead | undefined): Item[] {
  const row = { mine: false, changed: 0, note: '', model: '', nozzle: '', material: '', config: {} };
  if (!data) return [];
  if ('vendors' in data) return data.vendors.flatMap((v) => v.models.map((m): Item => {
    const head = { ...row, key: `catalog:machine:${v.id}/${m.id}`, name: m.id, vendor: v.id, model: m.id, cover: m.cover ?? null, catalog: m };
    return { ...head, name: modelLabel({}, v.id, m.id), fullName: m.id, meta: `${m.nozzles.join(', ')} mm`, variants: m.nozzles.map((n) => ({ ...head, key: `${head.key}@${n}`, nozzle: n })) };
  }));
  return data.products.map((p): Item => ({ ...row, key: `catalog:filament:${p.id}`, name: p.id, vendor: p.brand ?? '', material: p.type || 'Other',
    note: p.brand ?? '', catalog: p }));
}
function RemovedDetails({type,record}: {type:PresetType;record:ProfileRecord}) {
  const [busy,setBusy]=useState(false);
  const restore=async()=>{
    setBusy(true);
    try {
      const name = await restoreProfile(type, record);
      (name===record.name ? toast('restored', { name }) : toast('brought_back_as_since', { name, name2: record.name }));
    } catch(e) {failed('Restore',e);} finally {setBusy(false);}
  };
  return <DetailPane label={`Removed ${record.name}`} identity={<span className="pf-editor-name"><strong>{record.name}</strong></span>}
    summary={<div className="pf-editor-summary">Removed {new Date(record.archived!.at).toLocaleString([], {month:'short',day:'numeric',hour:'numeric',minute:'2-digit'})}</div>}
    controls={<button type="button" className="btn primary" disabled={busy} onClick={()=>void restore()}>{busy ? 'Restoring…' : 'Restore'}</button>}>
    <p className="opt-note">Restore pops it right back into your library with all its settings. Any slots it was loaded in stay as they are.</p>
  </DetailPane>;
}

function AddDetails({type,item,busy,onAdd}: {type:PresetType;item:Item;busy:boolean;onAdd(item:Item,nozzle?:string,firmware?:string):void}) {
  const load=useStore(s=>s.profileLibraryLoad.state);
  const firmwares=type === 'machine' ? (item.catalog as CatalogModel).firmwares ?? [] : [];
  const [firmware,setFirmware]=useState(firmwares[0]?.id);
  const nozzles=firmwares.find(f=>f.id === firmware)?.nozzles ?? (item.variants ?? []).map(v=>v.nozzle);
  const closest=(list:string[])=>[...list].sort((a,b)=>Math.abs(Number(a)-0.4)-Math.abs(Number(b)-0.4))[0];
  const [nozzle,setNozzle]=useState(()=>closest(nozzles));
  const pickFirmware=(f:string)=>{setFirmware(f);const list=firmwares.find(x=>x.id === f)?.nozzles ?? [];if(!list.includes(nozzle))setNozzle(closest(list));};
  const product=type === 'filament' ? item.catalog as CatalogEntry : undefined;
  const label=busy ? 'Adding…' : load === 'loading' ? 'Loading…' : `Add ${NOUNS[type][0]}`;
  return <DetailPane label={item.fullName ?? item.name} identity={<><span className="pf-editor-name"><strong>{item.fullName ?? item.name}</strong></span></>}
    summary={<div className="pf-editor-summary">{product ? [product.type, product.brand].filter(Boolean).join(SEP) : vendorLabel({},item.vendor)}</div>}
    controls={<>
      {firmwares.length > 1 && <Dropdown className="combo" aria-label="Firmware" value={firmware ?? ''} options={firmwares.map(f=>({value:f.id,label:firmwareName(f.id)}))} onValueChange={pickFirmware}/>}
      {nozzles.length > 1 && <Dropdown className="combo" aria-label="Nozzle" value={nozzle} options={nozzles.map(n=>({value:n,label:`${n} mm`}))} onValueChange={setNozzle}/>}
      <button type="button" className="btn primary" disabled={busy || load !== 'ready'} onClick={()=>onAdd(item,nozzle,firmwares.length ? firmware : undefined)}>{label}</button>
    </>}>
    <div className="pf-add-body">
      {item.cover && <img className="pf-add-cover" src={item.cover} alt="" width={240} height={240}/>}
    </div>
  </DetailPane>;
}

function configOf(s: State, type: PresetType, name: string) {
  const slot = slotsOf(s, type, name)[0];
  if (slot === undefined) return resolvePreset(s, type, name).config;
  const { config } = resolvePreset(s, type, name);
  const edits = type === 'filament' ? s.filamentEdits[name] : s.dirty[type];
  return { ...config, ...edits };
}

function Compare({ type, names, titles, close }: { type: PresetType; names: string[]; titles: string[]; close(): void }) {
  const ready = useTabLoaded(type);
  const [only, setOnly] = useState(true);
  useStore((s) => s.dirty);
  useStore((s) => s.filamentEdits);
  const s = getState(), resolved = useStore((st) => st.resolved);
  useAsks(names.filter((n) => !slotsOf(s, type, n).length).map((id) => ({ type, id })));
  const useSlot = names.length === 2 ? slotsOf(s, type, names[0])[0] : undefined;
  const rows = useMemo(() => (ready ? diffRows(type, names.map((n) => configOf(s, type, n)), !only) : []), [ready, type, names.join('\n'), only, s.dirty, s.filamentEdits, resolved]);
  const changed = rows.filter((r) => r.differs).length;
  const value = (r: (typeof rows)[number], i: number) => (r.values[i] ?? OPTIONS[r.key]?.default) as ConfigValue;
  let group = '';
  return (
    <fieldset className="pf-box pf-compare">
      <legend>Compare</legend>
      <div className="pf-compare-bar">
        <label className="row"><Check checked={only} onCheckedChange={(next) => setOnly(next)} />Only differences</label>
        <span className="pf-dim">{changed} different</span>
        <span className="spacer" />
        <button type="button" className="pf-link" onClick={close}>Close</button>
      </div>
      <div className="pf-scroll pf-diff">
        {!ready ? <div role="status">Loading…</div> : !rows.length ? <div className="pf-dim">No differences</div> : (
          <table className="pf-table">
            <thead><tr><th>Setting</th>{names.map((n, i) => <th key={n} className="n">{titles[i]}</th>)}{useSlot !== undefined && <th aria-label="Use" />}</tr></thead>
            <tbody>{rows.flatMap((r) => {
              const out = [];
              if (r.group !== group) { group = r.group; out.push(<tr key={`g:${group}`} className="pf-group"><td colSpan={names.length + (useSlot !== undefined ? 2 : 1)}>{group}</td></tr>); }
              out.push(
                <tr key={r.key} className={r.differs ? 'is-diff' : undefined}>
                  <td>{r.label}</td>
                  {names.map((n, i) => <td key={n} className="n">{display(r.key, value(r, i))}</td>)}
                  {useSlot !== undefined && <td>{r.differs && <button type="button" className="pf-link" onClick={() => takeValue(type, r.key, value(r, 1), useSlot)}>← Use</button>}</td>}
                </tr>,
              );
              return out;
            })}</tbody>
          </table>
        )}
      </div>
    </fieldset>
  );
}

function ListSkeleton({ type }: { type: PresetType }) {
  return <div className="pf-skeleton" role="status" aria-label="Loading profiles">{[0, 1, 2, 3, 4].map((i) => <div key={i} className="pf-item pf-skeleton-row" data-kind={type}>
    {type === 'machine' && <Skeleton block width={40} height={40}/>}
    <span className="pf-skeleton-text"><Skeleton width={`${60 - i * 6}%`} height={10}/>{type !== 'process' && <Skeleton width="35%" height={8}/>}</span>
  </div>)}</div>;
}

export function LibraryPage({ hidden }: { hidden: boolean }) {
  const request = useStore((s) => s.library);
  const { type: initialType, name, compare, edit = false, add = false, stage } = request;
  const libraryReady=useStore(s=>s.profileLibraryLoad.state==='ready');
  const [mobilePane,setMobilePane]=useState<'list'|'detail'|'filters'>(edit || stage || compare!==undefined?'detail':'list');
  const mobileBack=useRef<HTMLButtonElement>(null),mounted=useRef(true);
  const [paneTier,body]=useContainerTier(),drill=paneTier==='narrow' || paneTier==='phone';
  const [selecting,setSelecting]=useState(false),selectingRef=useRef(false);selectingRef.current=selecting;
  useEffect(()=>{mounted.current=true;return()=>{mounted.current=false;};},[]);
  const pickerAdd=useRef<PresetType|null>(add ? initialType : null);
  const firstRequest=useRef(request),wanted=useRef<string|null>(null),[wantTick,setWantTick]=useState(0);
  useEffect(()=>{if(drill && mobilePane==='filters')mobileBack.current?.focus();},[mobilePane]);
  const [type, setType] = useState(initialType), [search, setSearch] = useState(''), [adding, setAdding] = useState(add), [addMenu, setAddMenu] = useState(false);
  const [vendorFilter,setVendorFilter] = useState<string[]>([]), [nozzleFilter,setNozzleFilter] = useState<string[]>([]), [materialFilter,setMaterialFilter] = useState<string[]>([]), [modelFilter,setModelFilter]=useState<string[]>([]), [loadedFilter,setLoadedFilter]=useState<string[]>([]);
  const [filtersOpen,setFiltersOpen]=useState(false);
  const resultsPane = useRef<HTMLElement>(null);
  const drag = useRef<{id:number;x:number;width:number}|null>(null);
  const savedListWidth = usePreference('orca.profileListWidth');
  const minListWidth=260,maxListWidth=440,clampList=(width:number)=>Math.round(Math.max(minListWidth,Math.min(maxListWidth,width)));
  const listWidth = Number(savedListWidth) ? clampList(Number(savedListWidth)) : null;
  const [previewWidth,setPreviewWidth]=useState<number|null>(null);
  const renderedListWidth=previewWidth ?? listWidth;
  const shownListWidth=renderedListWidth ?? clampList(Math.min(1680,innerWidth-32)*.34);
  const resizeList = (width: number, commit: boolean) => {
    const next = clampList(width);
    setPreviewWidth(commit ? null : next);
    if(commit && next!==listWidth)savePreferences({'orca.profileListWidth':next});
  };
  const drafts = useStore(s => s.libraryDrafts);
  const setDrafts = (next: Record<string, ProfileDraft>) => setState({libraryDrafts: next});
  const updateDraft = updateSharedDraft;
  const users = useStore(st => st.records), health = useStore(st => st.libraryHealth), dirty = useStore(st => st.dirty), filamentEdits = useStore(st => st.filamentEdits);
  const printer = useStore(st => st.printer), process = useStore(st => st.process), slots = useStore(st => slotFilaments(st));
  const [reveal,setReveal]=useState<{key?:string}>({});
  const catalog = useCatalog(type, adding);
  const [showRemoved,setShowRemoved]=useState(false), removedCount=useStore(st=>((type === 'machine' ? st.records.printers : type === 'process' ? st.records.processes : st.records.filaments) as ProfileRecord[]).filter(r=>r.archived).length);
  const removedView=showRemoved && !adding && removedCount > 0;
  const list = useMemo(() => adding ? catalogRows(catalog.data) : items(getState(),type,removedView), [adding,catalog.data,type,users,health,dirty,filamentEdits,printer,process,slots,removedView]);
  const onPrinter=useOnPrinter(type === 'filament' && !adding);
  const [materialDefault,setMaterialDefault]=useState(false);
  const facets=useMemo(()=>{
    const flat=list.flatMap(i=>i.variants ?? [i]);
    const rows=flat.filter(i=>!vendorFilter.length || vendorFilter.includes(i.vendor));
    const machines=users.printers.filter(p=>!p.archived),contexts=machines.filter(p=>!modelFilter.length || modelFilter.includes(p.model));
    const unique=(values:string[])=>[...new Set(values.filter(Boolean))].sort();
    return {vendors:unique(flat.map(i=>i.vendor)),models:unique(type === 'machine' ? rows.map(i=>i.model) : machines.map(p=>p.model)),nozzles:unique(type === 'machine' ? rows.filter(i=>!modelFilter.length || modelFilter.includes(i.model)).map(i=>i.nozzle) : contexts.flatMap(p=>p.nozzles.map(n=>String(n.diameter)))),materials:unique(rows.map(i=>i.material))};
  },[list,type,users,vendorFilter,modelFilter]);
  useEffect(()=>{setNozzleFilter(prior=>{const next=prior.filter(n=>facets.nozzles.includes(n));return next.length === prior.length ? prior : next;});},[facets.nozzles]);
  const matching=useMemo(()=>{
    const materials=search.trim() && materialDefault ? [] : materialFilter;
    const out=list.filter(i=>{const variants=i.variants ?? [i];return (loadedFilter.length !== 1 || onPrinter.has(i.key) === (loadedFilter[0] === 'loaded')) && (!vendorFilter.length || vendorFilter.includes(i.vendor)) && (!materials.length || materials.includes(i.material)) && (type === 'machine' ? (!modelFilter.length || modelFilter.includes(i.model)) && (!nozzleFilter.length || variants.some(v=>nozzleFilter.includes(v.nozzle))) : true);});
    const pinned=reveal.key && !out.some(i=>i.key === reveal.key) ? list.find(i=>i.key === reveal.key) : undefined;
    return pinned ? [...out,{...pinned,pinned:true}] : out;
  },[list,type,vendorFilter,materialFilter,materialDefault,search,modelFilter,nozzleFilter,loadedFilter,onPrinter,users,printer,process,dirty,filamentEdits,reveal.key]);
  const query=search.trim().toLocaleLowerCase();
  const compatibleModels=(i:Item)=>{
    const found=new Map<string,{label:string}>(),st=getState(),product=i.record ? profileInfo(st,'filament',i.key).product : undefined,line=product ? loadedLine('filaments',product) : undefined;
    if(!line)return found;
    for(const p of st.records.printers)if(!p.archived && p.nozzles.some(n=>line.tunings.some(t=>t.fits.includes(n.variant))))found.set(p.id,{label:p.name});
    return found;
  };
  const machineRow=(i:Item):Item=>i.variants ? {...i,fullName:i.model,name:modelLabel(i.config,i.vendor,i.model)}
    : {...i,note:rowDetail(i.name,[{value:modelLabel(i.config,i.vendor,i.model)},nozzleField(first(i.config.nozzle_diameter))],[vendorLabel(i.config,i.vendor)])};
  const filamentRow=(i:Item,group:string):Item=>{
    if(i.variants)return i;
    const at=/\s*@.*$/.exec(i.name),scoped=false,models=[...compatibleModels(i).values()].map(m=>m.label);
    const title=at && (scoped || models.length) ? i.name.slice(0,at.index) : i.name;
    const tag=at && !scoped && models.length ? models[0]+(models.length > 1 ? ` +${models.length-1}` : '') : '';
    const on=onPrinter.get(i.key);
    return {...i,fullName:title === i.name ? undefined : i.name,name:title,note:i.mine ? '' : rowDetail(title,[{value:i.material},vendorField(i.vendor)],[group]),meta:[i.meta,tag].filter(Boolean).join(SEP) || undefined,
      ...(on ? {loaded:placementText(on)} : {})};
  };
  const groups=useMemo(():Group[]=>{
    const found=matching.filter(i=>!query || [i,...i.variants ?? []].some(v=>`${v.name} ${v.note}`.toLocaleLowerCase().includes(query)));
    const rows=found;
    const bucket=(list:Item[],labelOf:(i:Item)=>string)=>{const m=new Map<string,Item[]>();for(const i of list){const g=labelOf(i);m.set(g,[...m.get(g) ?? [],i]);}return [...m].sort(([a],[b])=>byLabel(a,b));};
    const sortRows=(list:Item[])=>list.sort((a,b)=>a.name.localeCompare(b.name));
    if(type !== 'process')return [...bucket(rows,i=>type === 'machine' ? vendorLabel(i.config,i.vendor) : i.material || 'Other').sort(([a],[b])=>type === 'filament' ? materialRank(a)-materialRank(b) || byLabel(a,b) : 0)
      .map(([label,list])=>({key:label,label,rows:adding ? sortRows(list) : type === 'machine' ? sortRows(list.map(machineRow)) : type === 'filament' ? (adding ? sortRows(list.map(i=>filamentRow(i,label))) : sortRows(list.map(i=>filamentRow(i,label)))) : sortRows(list)}))];
    const mine=rows.filter(i=>i.record).map(i=>({...i,fullName:i.name,note:'',meta:undefined,dynamic:isDynamic(i.record)}));
    return [{key:'mine',label:'My profiles',rows:sortRows(mine)}];
  },[matching,query,type,printer,onPrinter]);
  const visible=useMemo(()=>unique(groups.flatMap(g=>g.rows)),[groups]);
  const matched=visible.filter(i=>!i.pinned).length;
  const [folds,setFolds]=useState<ReadonlyMap<string,boolean>>(()=>new Map(sessionFolds)),[searchFolds,setSearchFolds]=useState<ReadonlySet<string>>(new Set());
  useEffect(()=>setSearchFolds(new Set()),[query]);
  const setFold=(key:string,open:boolean)=>{sessionFolds.set(key,open);setFolds(prior=>new Map(prior).set(key,open));};
  const filtersActive=vendorFilter.length+modelFilter.length+nozzleFilter.length+materialFilter.length+loadedFilter.length;
  const clearFilters=()=>{setVendorFilter([]);setModelFilter([]);setNozzleFilter([]);setMaterialFilter([]);setLoadedFilter([]);setMaterialDefault(false);};
  const defaulted=useRef('');
  useEffect(()=>{
    if(!adding){defaulted.current='';return;}
    const view=`${type}`;
    if(defaulted.current === view || !list.length)return;
    defaulted.current=view;
    const common=type === 'filament' ? COMMON_MATERIALS.filter(m=>facets.materials.includes(m)) : [];
    if(common.length){setMaterialFilter(common);setMaterialDefault(true);}
  },[adding,type,list]);
  const scope = `profiles:${adding ? 'add' : 'owned'}:${type}`, groupKey = (key: string) => `${scope}:${key}`;
  const printerModel=useMemo(()=>first(resolvePreset(getState(),'machine',printer).config.printer_model),[printer,users]);
  const addOpen = (g: Group) => type === 'filament' || g.label === vendorLabel({}, resolvePreset(getState(),'machine',printer).base?.vendor ?? '');
  const foldOpen = (g: Group) => folds.get(groupKey(g.key)) ?? (type === 'process' ? adding && (g.key === printerModel || groups.length === 1) : adding ? addOpen(g) : !adding || groupHas(g, r => r.slot !== undefined || r.key === name));
  const isOpen = (g: Group) => query ? !searchFolds.has(groupKey(g.key)) : foldOpen(g);
  const order = useMemo(() => {
    const keys: string[] = [], walk = (list: Group[]) => {for(const g of list)if(isOpen(g)){if(g.children)walk(g.children);else keys.push(...g.rows.map(i => i.key));}};
    walk(groups);
    return [...new Set(keys)];
  }, [groups, folds, searchFolds, query, scope, name, adding]);
  const current = (t: PresetType) => (t === 'machine' ? printer : t === 'process' ? process : slots[0]) ?? '';
  const [selectionState, setSelection] = useState(() => {
    let initial = createSelection(scope, order);
    const target = (name ? list.find(i => i.name === name || i.key === name) : undefined) ?? list.find(i => i.key === current(initialType) || i.name === current(initialType)) ?? list[0];
    if(target) initial = reduceSelection(initial, {type: 'CLICK', key: target.key});
    if(compare !== undefined) {const other = list.find(i => i.name === compare || i.key === compare) ?? list.find(i => i.key !== target?.key); if(other) initial = reduceSelection(initial, {type: 'CLICK', key: other.key, toggle: true});}
    return initial;
  });
  const previousScope = useRef(scope), initializedSelection = useRef(order.length > 0);
  const kept = useMemo(() => reveal.key && !order.includes(reveal.key) && visible.some(i => i.key === reveal.key) ? [...order, reveal.key] : order, [order, reveal.key, visible]);
  const selection = useMemo(() => reduceSelection(selectionState, {type: 'SYNC', scope, visible: kept}), [selectionState, scope, kept]);
  useEffect(() => {
    const changed=previousScope.current !== scope; previousScope.current=scope;
    const initialize = changed || !initializedSelection.current;
    if(order.length) initializedSelection.current=true;
    const want=wanted.current ? visible.find(i=>(i.key === wanted.current || i.name === wanted.current || i.fullName === wanted.current) && kept.includes(i.key)) : undefined;
    if(want){wanted.current=null;if(!adding)setReveal({key:want.key});setSelection(reduceSelection(createSelection(scope,kept),{type:'CLICK',key:want.key}));return;}
    setSelection(prior=>{
      let next=reduceSelection(prior,{type:'SYNC',scope,visible:kept});
      if(visible.length && next.selected.length === 0 && (initialize || prior.selected.length > 0 && !prior.selected.some(key=>drafts[JSON.stringify([type,key])]))) {
        const selectedKey=current(type), target=visible.find(i=>i.key === name || i.name === name || i.fullName === name) ?? visible.find(i=>i.key === selectedKey || i.name === selectedKey) ?? visible[0];
        next=reduceSelection(next,{type:'CLICK',key:target.key});
        if(!adding)setReveal({key:target.key});
      }
      return next;
    });
  },[scope,kept,visible.length,wantTick]);
  const selected = selectedVisible(selection), selectedSet = useMemo(() => new Set(selected), [selection]);
  const selectedItems=visible.filter(i=>selectedSet.has(i.key));
  const inspected=useRef<string|null>(null);
  const filteredDraft=inspected.current && !order.includes(inspected.current) && drafts[JSON.stringify([type,inspected.current])] ? list.find(i=>i.key === inspected.current) : undefined;
  const item=filteredDraft ?? (selectedItems.length === 1 ? selectedItems[0] : undefined);
  if(item && !filteredDraft)inspected.current=item.key;
  const latestSelection = useRef(selection); latestSelection.current = selection;
  const selectionRevision=useRef(0);
  const id = useId();
  void edit;
  const [compareHidden, setCompareHidden] = useState<string | null>(null);
  const ops = useMachine(windowOps), busy = ops.value !== 'idle', pendingCopy = ops.context.copy;
  const run = async (op: WindowOp, task: () => Promise<void>) => {
    if(!windowOps.can({type: 'BEGIN', op})) return;
    windowOps.send({type: 'BEGIN', op});
    try {await task();} finally {windowOps.send({type: 'END', op});}
  };
  const editorSaving = useCallback((saving: boolean) => {
    if(saving) {if(windowOps.can({type: 'BEGIN', op: 'editor-saving'})) windowOps.send({type: 'BEGIN', op: 'editor-saving'});}
    else if(windowOps.get().value === 'editor-saving') windowOps.send({type: 'END', op: 'editor-saving'});
  }, []);
  const [bulkError, setBulkError] = useState<string | null>(null);
  const [contextMenu,setContextMenu] = useState<{x:number;y:number;anchor:HTMLElement}|null>(null);
  const fallbackKey=useRef<()=>string|undefined>(()=>undefined);
  fallbackKey.current=()=>{if(adding)return undefined;const used=current(type);return (visible.find(i=>i.key === used || i.name === used) ?? visible[0])?.key;};
  const dispatch = useCallback((event: SelectionEvent, focus = false) => {
    selectionRevision.current++;initializedSelection.current=true;inspected.current=null;
    if(selectingRef.current && event.type === 'CLICK')event={...event,toggle:true};
    let next = reduceSelection(latestSelection.current, event);
    const fallback = next.selected.length ? undefined : fallbackKey.current();
    if(fallback) {next = reduceSelection(next, {type: 'CLICK', key: fallback}); setReveal({key: fallback});}
    latestSelection.current = next; setSelection(next);
    if(event.type==='CLICK' && !event.toggle && !event.shift)setMobilePane('detail');
    if(focus && next.focus) requestAnimationFrame(() => {const row = document.getElementById(`${id}:${next.focus}`); row?.focus({preventScroll: true}); row?.scrollIntoView({block: 'nearest'});});
  }, [id]);
  const openRow=adding ? undefined : item?.key;
  useEffect(() => {if(openRow !== reveal.key)setReveal({key: openRow});}, [openRow]);
  const clickedIn=useRef<string|undefined>(undefined), [openIn,setOpenIn]=useState<string>(), revealed=useRef<string|undefined>(undefined);
  const revealPath=(key:string)=>{
    const paths:Group[][]=[],walk=(list:Group[],at:Group[])=>{for(const g of list){if(g.children)walk(g.children,[...at,g]);else if(g.rows.some(r=>r.key === key))paths.push([...at,g]);}};
    walk(groups,[]);
    const path=paths.find(p=>p[p.length-1].key === clickedIn.current) ?? paths.find(p=>p.every(foldOpen)) ?? paths.find(p=>p[0].key === printerModel) ?? paths[0];
    clickedIn.current=undefined;
    if(!path)return false;
    setOpenIn(path[path.length-1].key);
    if(!path.every(foldOpen))setFolds(prior=>{const next=new Map(prior);for(const g of path)next.set(groupKey(g.key),true);return next;});
    return true;
  };
  useEffect(()=>{if(!openRow || query || revealed.current === `${scope}:${openRow}`)return;if(revealPath(openRow))revealed.current=`${scope}:${openRow}`;},[openRow,groups,query,scope]);
  useEffect(() => {const key=item?.key; if(key) requestAnimationFrame(() => document.getElementById(`${id}:${key}`)?.scrollIntoView({block: 'nearest'}));}, [item?.key, type, adding, !query]);
  const [renameRequest,setRenameRequest]=useState<{key:string;at:number}>();
  const requestRename=(key:string)=>{if(adding)return;if(item?.key !== key)dispatch({type:'CLICK',key});setRenameRequest({key,at:Date.now()});};
  const leave = async (action: () => void) => {if(Object.keys(drafts).length && !await confirmDialog('Unsaved profile edits', 'Discard unsaved profile edits?', ['Discard', 'Keep editing'])) return; action();};
  const close=()=>{if(!busy)void leave(()=>setState({tab:'prepare'}));};
  const pickType = (t: PresetType, target?: string) => {
    selectionRevision.current++;
    setType(t); setSearch(''); setVendorFilter([]); setNozzleFilter([]); setMaterialFilter([]); setModelFilter([]); setBulkError(null);
    const rows = adding ? [] : items(getState(), t), nextScope = `profiles:${adding ? 'add' : 'owned'}:${t}`;
    let next = createSelection(nextScope, rows.map(i => i.key));
    const row = rows.find(i => i.key === target || i.name === target);
    if(row) next = reduceSelection(next, {type: 'CLICK', key: row.key});
    setSelection(next);
    wanted.current = row?.key ?? (target || current(t) || null); setWantTick((n) => n + 1);
  };
  useEffect(()=>{if(!request || request === firstRequest.current)return;firstRequest.current=request;void leave(()=>{
    setAdding(!!add);pickerAdd.current=add ? initialType : null;wanted.current=name ?? null;setWantTick(t=>t+1);pickType(initialType,name);setMobilePane(edit || stage || compare!==undefined?'detail':'list');});},[request]); // eslint-disable-line react-hooks/exhaustive-deps
  const added = (n: string,targetType=type) => {setType(targetType); setAdding(false); setMobilePane('detail');setSearch(''); setVendorFilter([]); setNozzleFilter([]); setMaterialFilter([]); const p = findProfile(getState(), targetType, n); if(p) {const key = profileKey(p); const initial = createSelection(`profiles:owned:${targetType}`, [key]); setSelection(reduceSelection(initial,{type: 'CLICK',key})); setReveal({key});}};
  const consumeCopy=async(captured:CopyRequest,owns:()=>boolean)=>{
    const key=await captured.creation.commit();
    if(windowOps.get().context.copy===captured)windowOps.send({type:'COPY_DONE'});
    if(owns())added(key,captured.source.type);else toast('added', { name: captured.name });
  };
  const owner=()=>{
    const revision=selectionRevision.current,document=documentGeneration(),before=getState();
    return ()=>mounted.current && documentGeneration()===document && getState().api===before.api && selectionRevision.current===revision
      && getState().printer===before.printer && getState().process===before.process && slotFilaments(getState())===slotFilaments(before);
  };
  const addFromCatalog = async (row:Item,nozzle?:string,firmware?:string) => {
    if(getState().profileLibraryLoad.state!=='ready' || !row.catalog)return;
    await run('adding', async () => {
    setBulkError(null);
    const owns=owner();
    try {
      let key:string,restored:string|undefined;
      if(type === 'machine'){
        const model=row.catalog as CatalogModel,result=await addCatalogPrinters([{vendor:row.vendor,model,nozzle:nozzle ?? '',firmware}]);
        if(!result.keys.length)throw new Error(`${result.skipped[0]} is already in your printers list.`);
        key=result.keys[0];restored=result.restored?.[0];
        if(restored)toast('brought_back_slots_settings', { restored }, { action: {label:'Add a new one',run:()=>void addCatalogPrinter(row.vendor,model,nozzle ?? '','another',firmware).then(k=>{if(mounted.current)added(k);}).catch(e=>toast('couldnt_add_another', { name: row.name, detail: e instanceof Error?e.message:String(e) }))} });
      } else key=await addCatalogProduct(row.catalog as CatalogEntry);
      if(!owns()){if(!restored)toast('added', { name: row.name });return;}
      added(key);
      if(pickerAdd.current === type){pickerAdd.current=null;requestPreset(type,0,key);}
    }
    catch(error) {if(mounted.current)setBulkError(error instanceof Error?error.message:String(error));}
    });
  };
  const addSelectedFromCatalog = async () => {
    const rows=selectedItems.filter(i=>i.catalog);
    if(getState().profileLibraryLoad.state!=='ready' || !rows.length)return;
    await run('adding', async () => {
      setBulkError(null);
      try {
        const {keys,skipped}=type === 'machine'
          ? await addCatalogPrinters(rows.map(r=>{const m=r.catalog as CatalogModel;return {vendor:r.vendor,model:m,nozzle:m.nozzles.includes('0.4') ? '0.4' : m.nozzles[0]};}))
          : await addCatalogProducts(rows.map(r=>({product:r.catalog as CatalogEntry})));
        toast('catalog_rows_added', { added: keys.length ? count(keys.length,nouns) : '', skipped: skipped.join(', ') });
        if(keys.length && mounted.current)added(keys[0]);
      }
      catch(error) {if(mounted.current)setBulkError(error instanceof Error?error.message:String(error));}
    });
  };
  const retryCopy=async()=>{
    const copy=windowOps.get().context.copy;if(!copy || !libraryReady)return;
    let taken=false;
    await run('copying', async () => {
      try{await consumeCopy(copy,owner());}
      catch(error){
        if((error as {code?:string}).code==='name_taken'){taken=true;if(libraryWrites.can({type:'DISMISS'}))libraryWrites.send({type:'DISMISS'});}
        else windowOps.send({type:'COPY_FAILED',error:error instanceof Error?error.message:String(error)});
      }
    });
    if(taken){discardCopy();await duplicate(copy.source.id,copy.name);}
  };
  const duplicate=async(key:string,wanted?:string)=>{
    const source=findProfile(getState(),type,key);
    if(busy || !libraryReady || !source)return;
    if(pendingCopy){if(pendingCopy.source.id===source.id)await retryCopy();else setBulkError(`Please finish saving or discard your copy of ${pendingCopy.source.name} first.`);return;}
    const copyName=await promptDialog(`Duplicate ${NOUNS[type][0]}`,wanted ?? `${source.name} copy`,name=>newProfileNameError(getState(),type,name));
    if(!copyName || windowOps.get().context.copy || windowOps.get().value !== 'idle')return;
    windowOps.send({type:'COPY',source,name:copyName,creation:copyRecord(type === 'machine' ? 'printer' : type,source.id,copyName)});
    await retryCopy();
  };
  const discardCopy=()=>{if(windowOps.can({type:'DISCARD'}))windowOps.send({type:'DISCARD'});};
  const exportSelected = () => run('exporting', async () => {
    setBulkError(null);
    try {
      const kind = type === 'machine' ? 'printer' as const : type;
      const keys = selectedVisible(latestSelection.current);
      if(keys.length) await exportProfiles({kind, ids: keys});
    } catch(e) {setBulkError(e instanceof Error ? e.message : String(e));}
  });
  const deleteSelected = (keys?:string[]) => run('deleting', async () => {
    setBulkError(null);
    try {await archiveRecords(type,keys ?? selectedVisible(latestSelection.current));}
    catch(error) {setBulkError(error instanceof Error ? error.message : String(error));}
  });
  const targetRequest=useRef({type:initialType,name});
  useEffect(()=>{
    const prior=targetRequest.current;targetRequest.current={type:initialType,name};
    if(prior.type === initialType && prior.name === name || initialType === type && (name === item?.key || name === item?.name))return;
    setAdding(!!add);pickType(initialType,name);
  },[initialType,name]);
  useEffect(()=>{if(!drill || mobilePane!=='detail')return;const pane=resultsPane.current?.parentElement?.querySelector<HTMLElement>(':scope > .pf-detail-pane'),sheet=pane?.parentElement;
    if(pane && sheet && pane.getBoundingClientRect().top > sheet.getBoundingClientRect().bottom - 120)pane.scrollIntoView({block:'start',behavior:'smooth'});},[mobilePane,item?.key]);
  const backToList=()=>{setMobilePane('list');setFiltersOpen(false);requestAnimationFrame(()=>document.getElementById(`${id}:${latestSelection.current.focus}`)?.focus());};
  const nouns=NOUNS[type], typeLabel=type === 'process' ? nouns[1] : nouns[0];
  const resultNoun:[string,string]=adding && type === 'machine' ? ['model','models'] : nouns;
  const pendingSet=useRef<{keys:string[];on:boolean}|null>(null);
  useEffect(()=>{const p=pendingSet.current;if(p && p.keys.every(k=>order.includes(k))){pendingSet.current=null;dispatch({type:'SET',keys:p.keys,on:p.on});}},[order]);
  const selectGroup=(g:Group,on:boolean)=>{const keys=g.rows.map(r=>r.key);if(isOpen(g))dispatch({type:'SET',keys,on});else{pendingSet.current={keys,on};toggleGroup(g);}};
  const toggleGroup=(g:Group)=>{const k=groupKey(g.key);if(query)setSearchFolds(prior=>{const next=new Set(prior);if(!next.delete(k))next.add(k);return next;});else setFold(k,!isOpen(g));};
  useEffect(()=>{const key=selection.focus;if(query && key && revealPath(key))revealed.current=`${scope}:${key}`;},[selection.focus]);
  const renderGroup=(g:Group,depth:number):React.ReactNode=>{
    const flat=type === 'process',open=flat || isOpen(g);
    return <div key={g.key} role="rowgroup" className="pf-group" data-depth={depth} data-flat={flat || undefined}>
      {!flat && <div role="row" className="pf-group-head">{(()=>{const n=g.rows.filter(r=>selectedSet.has(r.key)).length;
        return <span role="gridcell" className="pf-group-check"><Check aria-label={`Select all ${g.label}`} checked={n === 0 ? false : n === g.rows.length ? true : 'indeterminate'} onCheckedChange={()=>selectGroup(g,n < g.rows.length)}/></span>;})()}<span role="rowheader"><button type="button" className="pf-group-toggle" aria-expanded={open} onClick={()=>toggleGroup(g)}><Caret dir={open ? 'down' : 'right'}/><span className="pf-group-label">{g.label}</span>{g.sub && <span className="pf-dim">{g.sub}</span>}</button></span></div>}
      {open && (g.children ? g.children.map(c=>renderGroup(c,depth+1)) : <div role="presentation" className="pf-group-rows" onClickCapture={()=>{clickedIn.current=g.key;setOpenIn(g.key);}}>{g.rows.map(i => <ManagerProfileRow key={i.key} item={i} listId={id} selected={selectedSet.has(i.key)} open={i.key === openKey && (!openIn || openIn === g.key || !groups.some(o=>o.key === openIn && groupHas(o,r=>r.key === i.key)))} focused={selection.focus === i.key} selectable dispatch={dispatch}/>)}</div>)}
    </div>;
  };
  const handlers=useRef({close,addFromCatalog,addSelected:addSelectedFromCatalog,deleteSelected,duplicate});handlers.current={close,addFromCatalog,addSelected:addSelectedFromCatalog,deleteSelected,duplicate};
  const editKey=!adding && !removedView && item?.record ? item.key : undefined,editUser=editKey ? item?.record : undefined;
  const deltaKey=useStore(s=>editKey ? projectDeltaKey(s,type,editKey) : '');
  const answer=useStore(s=>s.resolved.answer);
  const draft=useMemo(()=>editKey ? sharedDraft(getState(),type,editKey,deltaKey,drafts[JSON.stringify([type,editKey])]) : undefined,[editKey,type,deltaKey,drafts,users,answer]);
  const editor=useMemo(()=>editKey && editUser && <ProfileEditor key={editKey} type={type} name={editKey} draft={draft} update={updateDraft} onSavingChange={editorSaving} onBack={()=>handlers.current.close()} onRemove={() => void handlers.current.deleteSelected([editKey])} onDuplicate={() => void handlers.current.duplicate(editKey)} foldActions={drill} renameRequest={renameRequest}/>,[editKey,editUser,type,draft,drill,updateDraft,renameRequest]);
  const addDetails=useMemo(()=>adding && item ? <AddDetails key={item.key} type={type} item={item} busy={busy} onAdd={(row,nozzle,firmware)=>void handlers.current.addFromCatalog(row,nozzle,firmware)}/> : null,[adding,item,type,busy]);
  const bulk=selected.length > 1, selectionKey=selected.join('\n');
  const canCompare=bulk && !adding && selected.length <= 4 && selectedItems.length === selected.length, comparing=canCompare && compareHidden !== selectionKey;
  const openKey=comparing ? undefined : item?.key;
  const bulkActions=<><button type="button" className="btn-ghost" aria-pressed={comparing} disabled={!canCompare || busy} onClick={() => setCompareHidden(comparing ? selectionKey : null)}>Compare</button>
    <button type="button" className="btn-ghost" data-output disabled={busy} onClick={() => void exportSelected()}>Export</button>
    <button type="button" className="btn-ghost pf-delete" disabled={busy} onClick={() => void deleteSelected()}>Delete</button></>;
  const shownSelected=order.filter(k=>selectedSet.has(k)).length;
  const selectAll=order.length > 1 && <span className="pf-select-all"><Check aria-label={`Select all ${order.length} shown`} checked={bulk ? shownSelected === order.length ? true : 'indeterminate' : false}
    onCheckedChange={()=>dispatch(bulk && shownSelected === order.length ? {type:'CLEAR'} : {type:'SELECT_ALL'})}/></span>;
  const shows=(n:number,v:string[])=>n > 1 || v.length > 0;
  const railFilters=type === 'filament' && (shows(facets.materials.length,materialFilter) || !adding && (onPrinter.size > 0 || loadedFilter.length > 0))
    || !adding && removedCount > 0 || shows(facets.vendors.length,vendorFilter) || type === 'machine' && (!adding && shows(facets.models.length,modelFilter) || shows(facets.nozzles.length,nozzleFilter));
  const noRail=!railFilters && !adding;
  const addActions=<button type="button" className="btn primary" disabled={busy || !libraryReady} onClick={() => void handlers.current.addSelected()}>Add {selected.length}</button>;
  const dismiss=selecting ? <button type="button" className="btn-ghost" onClick={()=>{setSelecting(false);dispatch({type:'CLEAR'});}}>Done</button>
    : <button type="button" className="icon-btn" aria-label="Clear selection" onClick={() => dispatch({type: 'CLEAR'})}><Glyph name="close" size={14} /></button>;
  useEffect(()=>{setSelecting(false);},[type,adding]);
  const showLibrary=(master:boolean)=>{if(master === adding || master && type === 'process')return;void leave(()=>{setDrafts({});clearFilters();setSearch('');setAdding(master);setMobilePane('list');});};
  const escapeAdd=useRef(()=>{});escapeAdd.current=()=>{if(!busy)showLibrary(false);};
  useEffect(()=>{if(hidden || !adding)return;const key=(e:KeyboardEvent)=>{const t=e.target instanceof Element ? e.target : null;if(e.key === 'Escape' && !e.defaultPrevented && !t?.closest('.pf-page, dialog, [role=dialog], [role=menu], [role=listbox]'))escapeAdd.current();};
    document.addEventListener('keydown',key);return ()=>document.removeEventListener('keydown',key);},[hidden,adding]);
  return <section className="pf-window pf-page" aria-label="Library" hidden={hidden} onKeyDown={event=>{if(event.key!=='Escape')return;if(busy || (event.target as Element | null)?.closest?.('.pf-name-input')){event.preventDefault();return;}
    const act=adding ? (selected.length > 1 ? ()=>dispatch({type:'ESCAPE'}) : ()=>showLibrary(false)) : drill && mobilePane!=='list' ? backToList : selected.length > 1 ? ()=>dispatch({type:'ESCAPE'}) : null;
    if(act){event.preventDefault();act();}}}>
    <header className="pf-workspace-title">
      <h1>Library</h1>
      <Segmented label="Profile type" value={type} options={TYPES.map(([id,label])=>({id,label}))} onChange={v=>void leave(()=>{setDrafts({});clearFilters();pickType(v as PresetType);})}/>
      <SettingsDetail compact/>
    </header>
    <ProfileLibraryNotice/>
    {!adding && <RemovedElsewhere type={type} onRestored={key=>dispatch({type:'CLICK',key})}/>}
    {pendingCopy && <div className="pf-copy-pending" role={pendingCopy.error ? 'alert' : 'status'}>
      <strong>{pendingCopy.name}</strong><span>{pendingCopy.error ?? 'Finish duplicating profile first'}</span>
      <button type="button" className="btn" disabled={busy || !libraryReady} onClick={()=>void retryCopy()}>Retry</button>
      {!creationPending(pendingCopy.creation) && <button type="button" className="btn-ghost" disabled={busy} onClick={discardCopy}>Discard</button>}
    </div>}
    <div className="np-dialog-body" ref={body}>
      <div className={`pf-split${filtersOpen ? ' filters-open' : ''}`} data-library={adding ? 'master' : 'mine'} data-rail={noRail ? 'none' : undefined} data-type={type} data-mobile-pane={mobilePane} style={renderedListWidth === null ? undefined : {'--profile-list-width': `${renderedListWidth}px`} as React.CSSProperties} inert={busy}>
      <div className="pf-mobile-navigation"><button ref={mobileBack} type="button" className="btn-ghost pf-back" onClick={backToList}><Glyph name="back"/>Back</button><strong>{mobilePane === 'filters' ? 'Filters' : item?.name ?? ''}</strong></div>
      <aside id={`${id}-filters`} className="pf-facets facets" aria-label="Library and filters">
        {railFilters && <FacetsHead active={filtersActive} onClear={clearFilters}/>}
        {type === 'filament' && <FacetGroup label="Material" options={[...facets.materials].sort((a,b)=>materialRank(a)-materialRank(b) || a.localeCompare(b)).map(value=>({value,label:value}))} value={materialFilter} onChange={next=>{setMaterialFilter(next);setMaterialDefault(false);}}/>}
        {type !== 'process' && <FacetGroup label="Brand" options={facets.vendors.map(value=>({value,label:type === 'filament' ? value : vendorLabel({},value)}))} value={vendorFilter} onChange={setVendorFilter}/>}
        {type === 'machine' && !adding && <FacetGroup label="Printer model" options={facets.models.map(value=>({value,label:value}))} value={modelFilter} onChange={setModelFilter}/>}
        {type === 'machine' && <FacetGroup label="Nozzle" options={facets.nozzles.map(value=>({value,label:`${value} mm`}))} value={nozzleFilter} onChange={setNozzleFilter}/>}
        {type === 'filament' && !adding && (onPrinter.size > 0 || loadedFilter.length > 0) && <FacetGroup label="On printer" options={[{value:'loaded',label:'Loaded'},{value:'unloaded',label:'Not loaded'}]} value={loadedFilter} onChange={setLoadedFilter}/>}
        {!adding && removedCount > 0 && <FacetGroup label="Removed" options={[{value:'removed',label:`Removed ${nouns[1]} (${removedCount})`}]} value={removedView ? ['removed'] : []} onChange={v=>setShowRemoved(v.length > 0)} toggle/>}
        <button type="button" className="btn pf-mobile-show-results" onClick={backToList}>Show {count(matched,resultNoun)}</button>
      </aside>
      <section className="pf-my-profiles" ref={resultsPane} aria-label={adding ? `Add ${typeLabel}` : `My ${nouns[1]}`}>
        <div className="pf-list-head"><div className="pf-results-heading" data-selection={bulk || undefined}>{(selecting || bulk) && selectAll}{bulk ? <><strong>{selected.length} selected</strong>{dismiss}<span className="pf-bulk-actions">{adding ? addActions : bulkActions}</span></>
          : <><span className="pf-results-count">{count(matched,resultNoun)}</span><span className="pf-results-actions">
          {adding ? <button type="button" className="btn" onClick={()=>showLibrary(false)}>Cancel</button>
            : type === 'process' ? <AddPrintMenu open={addMenu} onOpenChange={setAddMenu} onOpen={key=>added(key,'process')} onCreated={key=>added(key,'process')}><button type="button" className="btn slant" aria-label="Add print settings" disabled={!libraryReady || busy}>Add</button></AddPrintMenu>
            : <button type="button" className="btn slant" aria-label={`Add ${typeLabel}`} onClick={()=>showLibrary(true)}>Add</button>}
          {!adding && visible.length > 1 && <button type="button" className="btn slant" aria-pressed={selecting} onClick={()=>setSelecting(!selecting)}>{selecting ? 'Done' : 'Select'}</button>}</span></>}
        </div>
        <div className="pf-results-tools"><input className="text pf-profile-search" type="search" aria-label="Search" placeholder="Search" value={search} onChange={e=>setSearch(e.target.value)}/>
          <button type="button" hidden={type === 'process'} className="icon-btn pf-filter-toggle" aria-label={filtersActive ? `Filters, ${filtersActive} active` : 'Filters'} aria-expanded={filtersOpen || mobilePane==='filters'} aria-controls={`${id}-filters`} onClick={()=>{setFiltersOpen(!filtersOpen);setMobilePane(mobilePane==='filters'?'list':'filters');}}><Glyph name="filter"/>{filtersActive ? <span className="pf-filter-count" aria-hidden>{filtersActive}</span> : null}</button>
          {adding ? type === 'machine' && <MenuButton label="More ways to add" className="icon-btn" items={[{label:'Custom printer',action:()=>setState({appDialog:'createPrinter'})}]}><Glyph name="more"/></MenuButton>
            : <MenuButton label="Import, export or reset profiles" className="icon-btn" items={['sep',{label:'Import',action:()=>void leave(()=>setState({presetUx:{kind:'import'}}))},{label:'Export',output:true,action:()=>void leave(()=>setState({presetUx:{kind:'export'}}))}]}><Glyph name="more"/></MenuButton>}
        </div></div>
        <div className="pf-scroll pf-list" data-kind={type} data-multi={bulk || selecting || undefined} role="grid" aria-label={adding ? `Add ${typeLabel}` : `My ${nouns[1]}`} aria-multiselectable onDoubleClick={e=>{const row=(e.target as Element).closest<HTMLElement>('[data-profile-key]');if(row && (e.target as Element).closest('.pf-item-name'))requestRename(row.dataset.profileKey!);}} onContextMenu={e=>{const row=(e.target as Element).closest<HTMLElement>('[data-profile-key]');if(!row || adding) return;e.preventDefault();const key=row.dataset.profileKey!;if(!selectedSet.has(key)) dispatch({type:'CLICK',key});setContextMenu({x:e.clientX,y:e.clientY,anchor:row});}} tabIndex={visible.length ? -1 : 0} onKeyDown={e => {
          if(e.target instanceof Element && e.target.closest('input,textarea,[contenteditable=true]')) return;
          const command = e.ctrlKey || e.metaKey;
          if(command && e.key.toLocaleLowerCase() === 'a') {e.preventDefault(); e.stopPropagation(); dispatch({type: 'SELECT_ALL'});}
          else if(['ArrowDown','ArrowUp','Home','End'].includes(e.key)) {e.preventDefault(); e.stopPropagation(); dispatch({type: 'ARROW', direction: e.key === 'Home' ? 'first' : e.key === 'End' ? 'last' : e.key === 'ArrowUp' ? 'previous' : 'next', shift: e.shiftKey, focusOnly: command}, true);}
          else if(e.key === 'F2' && e.target instanceof HTMLElement && e.target.dataset.profileKey) {e.preventDefault(); requestRename(e.target.dataset.profileKey);}
          else if(e.key === 'Delete' || e.key === 'Backspace') {e.preventDefault(); e.stopPropagation(); if(!adding) void deleteSelected();}
          else if((e.key === 'Enter' || e.key === ' ') && e.target instanceof HTMLElement && e.target.dataset.profileKey) {e.preventDefault(); dispatch({type: 'CLICK', key: e.target.dataset.profileKey, toggle: command || e.key === ' ', shift: e.shiftKey});}
        }}>
          {!libraryReady && !adding ? <ListSkeleton type={type}/> : groups.map(g => renderGroup(g, 0))}
          {libraryReady && !visible.length && <div className="pf-profile-empty">{search || filtersActive ? <><span>No {resultNoun[1]} match</span><button type="button" className="btn-ghost" onClick={()=>{clearFilters();setSearch('');}}>Clear filters</button></>
            : adding ? <span>{catalog.error ?? (catalog.data ? 'Nothing to add' : 'Loading…')}</span> : <><span>No {nouns[1]} yet</span>{type !== 'process' && <button type="button" className="btn" onClick={()=>showLibrary(true)}>Add {typeLabel}</button>}</>}</div>}
        </div>
      </section><div className="pf-resizer" role="separator" aria-label="Resize profile list" aria-orientation="vertical" aria-valuemin={minListWidth} aria-valuemax={maxListWidth} aria-valuenow={shownListWidth} tabIndex={0}
        onPointerDown={e => {if(e.button!==0 || !resultsPane.current)return;e.preventDefault();drag.current={id:e.pointerId,x:e.clientX,width:resultsPane.current.getBoundingClientRect().width};e.currentTarget.setPointerCapture(e.pointerId);}}
        onPointerMove={e => {const start=drag.current;if(start?.id===e.pointerId && e.currentTarget.hasPointerCapture(e.pointerId))resizeList(start.width+e.clientX-start.x,false);}}
        onPointerUp={e => {const start=drag.current;if(start?.id===e.pointerId && e.currentTarget.hasPointerCapture(e.pointerId)){drag.current=null;if(e.currentTarget.getClientRects().length)resizeList(start.width+e.clientX-start.x,true);else setPreviewWidth(null);e.currentTarget.releasePointerCapture(e.pointerId);}}}
        onPointerCancel={e => {drag.current=null;setPreviewWidth(null);if(e.currentTarget.hasPointerCapture(e.pointerId))e.currentTarget.releasePointerCapture(e.pointerId);}}
        onLostPointerCapture={()=>{drag.current=null;setPreviewWidth(null);}}
        onKeyDown={e => {if(['ArrowLeft','ArrowRight','Home','End'].includes(e.key)) {e.preventDefault();resizeList(e.key==='Home'?minListWidth:e.key==='End'?maxListWidth:(resultsPane.current?.getBoundingClientRect().width ?? shownListWidth)+(e.key==='ArrowRight'?16:-16),true);}}}/><div className="pf-detail-pane">{filteredDraft && <div className="pf-filtered-draft" role="status"><span>Hidden by filters</span><button type="button" className="more-toggle" onClick={()=>{clearFilters();setSearch('');}}>Clear filters</button></div>}
        {adding && bulk ? <section className="pf-bulk-summary"><strong>{count(selected.length,type === 'machine' ? ['printer model','printer models'] : ['filament','filaments'])} selected</strong><div className="pf-bulk-actions">{addActions}</div></section>
        : adding ? addDetails
        : comparing ? <Compare type={type} names={selectedItems.map(i => i.key)} titles={selectedItems.map(i => i.name)} close={() => setCompareHidden(selectionKey)}/>
        : removedView && item?.record ? <RemovedDetails key={item.key} type={type} record={item.record}/>
        : item?.record ? editor
        : bulk ? <section className="pf-bulk-summary"><strong>{count(selected.length,nouns)} selected</strong><div className="pf-bulk-actions">{bulkActions}</div></section>
        : <p className="opt-note pf-detail-empty">{order.length ? `Pick a ${nouns[0]} to check out its settings.` : type === 'process' ? 'Import a print profile, or save one straight from Print settings in Prepare!' : `Add a ${typeLabel} to see its settings here!`}</p>}
      </div></div>

      {contextMenu && <ActionMenu label="Profile actions" at={contextMenu} returnFocus={contextMenu.anchor} onClose={()=>setContextMenu(null)} items={[...(selected.length === 1 && selectedItems[0]?.record ? [{label:'Rename',action:()=>requestRename(selectedItems[0].key),shortcut:'F2'}] : []),...(canCompare && !comparing ? [{label:'Compare',action:()=>setCompareHidden(null)}] : []),{label:'Export',output:true,action:()=>void exportSelected()},{label:'Delete',action:()=>void deleteSelected(),shortcut:'Delete'}]}/>}
      {bulkError && <div className="pf-editor-error" role="alert">{bulkError}</div>}
    </div>
  </section>;
}
