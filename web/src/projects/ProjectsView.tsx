import { bootFolders, bootProjects } from '../boot-shape';
import { HeldText, Skeleton } from '../components/Skeleton';
import { Component, useEffect, useId, useMemo, useRef, useState, useSyncExternalStore, type DragEvent, type KeyboardEvent, type MouseEvent, type ReactNode } from 'react';
import { ActionMenu, type MenuEntry } from '../components/Menu';
import { Check } from '../components/Check';
import { Dropdown } from '../components/Dropdown';
import { confirmDialog } from '../components/Ask';
import { toast } from '../state/store';
import { openWithUnsaved, projectListSnapshot, refreshProjects, subscribeProjectList, useProject } from '../flow/project';
import { useMachine } from '../flow/fsm';
import { projectsOps, runProjectsOp } from '../flow/projects-ops';
import { createSelection, reduceSelection, selectedVisible, type SelectionEvent } from '../profiles/selection';
import { deleteProject, duplicateProject, listTrash, openProject, useProjects, type ListedProject } from './api';
import { childFolders, createFolder, deleteFolder, FOLDER_NAME_MAX, folderPath, folderTree, moveFolder, moveProjects, renameFolder, useFolders, type Folder } from './folders';
import { fileVerbs, newProjectFrom } from './verbs';
import { setProjectsQuery, useProjectsQuery } from './query';
import { projectStore, type ProjectCommandStatus, type ProjectStore } from '../persist/backend';
import { preferenceJson, preferenceText, savedPreferencesSnapshot, savePreferences, subscribeSavedPreferences, usePreference } from '../saved-preferences';
import './projects.css';
import { Caret } from '../components/Caret';
import { createPortal } from 'react-dom';
import { usePhoneSheet } from '../workspace-sheet';
import { Sep } from '../separator';

const noCommands: readonly Readonly<ProjectCommandStatus>[] = Object.freeze([]);
const noSubscribe = () => () => {};
const emptyCommands = () => noCommands;
type ProjectUndo = {id:string;name:string;run:()=>Promise<void>};
const projectUndos = new Map<string,ProjectUndo>();
const trashRestores = new Map<string,number|undefined>();
const commandVerb = {rename:'Rename',duplicate:'Duplicate',archive:'Delete',restore:'Restore'};

type ProjectMeta = ListedProject;
type Place = {kind:'home'|'recent'|'favorites'|'unsorted'|'trash'} | {kind:'folder';id:string};
type Sort = 'edited'|'name'|'objects';
type View = 'grid'|'list';
const PLACES = [{kind:'home',label:'Home'},{kind:'recent',label:'Recent'},{kind:'favorites',label:'Favorites'},{kind:'unsorted',label:'Unsorted'}] as const;
const SORTS = [{value:'edited',label:'Last edited'},{value:'name',label:'Name'},{value:'objects',label:'Objects'}] as const;
const RECENT_MS = 7 * 86400_000, HOME_RECENT = 18;
const DRAG = 'application/x-orca-projects', FOLDER_DRAG = 'application/x-orca-folder';
let lastPlace: Place = {kind:'home'};
const collapsed = new Set<string>();

const ago = new Intl.RelativeTimeFormat(undefined, { numeric: 'auto', style: 'short' });
function edited(t: number) {
  if(!t)return '—';
  const s = (t - Date.now()) / 1000;
  if (s > -60) return ago.format(0, 'minute');
  if (s > -3600) return ago.format(Math.round(s / 60), 'minute');
  if (s > -86400) return ago.format(Math.round(s / 3600), 'hour');
  if (s > -7 * 86400) return ago.format(Math.round(s / 86400), 'day');
  return new Date(t).toLocaleDateString();
}
const objects = (n: number) => `${n} ${n===1?'object':'objects'}`;
const count = (n: number, one: string, many: string) => `${n} ${n===1?one:many}`;
const message = (error: unknown) => error instanceof Error ? error.message : String(error);
const names = (list: readonly {name:string}[]) => list.length <= 5 ? list.map(p=>p.name).join('\n') : `${list.slice(0,5).map(p=>p.name).join('\n')}\n+${list.length-5} more`;
const sorters: Record<Sort,(a:ProjectMeta,b:ProjectMeta)=>number> = {
  edited: (a,b)=>b.savedAt-a.savedAt,
  name: (a,b)=>a.name.localeCompare(b.name,undefined,{numeric:true,sensitivity:'base'}) || b.savedAt-a.savedAt,
  objects: (a,b)=>b.objects-a.objects || b.savedAt-a.savedAt,
};

async function exportProject(p:ProjectMeta) {
  if(!await confirmDialog('Export 3MF',`Open “${p.name}” and export it?`,['Export','Cancel']))return;
  try {const flow=await import('../flow/project');await flow.openProject(p.id);if(flow.project.get().context.id!==p.id || ['missing','failed','opening'].includes(flow.project.get().value))throw new Error('Oops! We couldn\'t open that project.');await (await import('../actions')).saveProject();}
  catch(error){toast('export_failed', { detail: message(error) });}
}

function Thumb({p}:{p:ProjectMeta}) {
  return p.thumbnail && p.objects > 0 ? <img src={p.thumbnail} alt="" draggable={false} loading="lazy" decoding="async"/>
    : <span className="pj-thumb-empty">{p.objects > 0 ? 'No preview' : 'Empty'}</span>;
}
const Dots = () => <svg viewBox="0 0 24 24" width="16" height="16" aria-hidden><circle cx="5" cy="12" r="1.8" fill="currentColor"/><circle cx="12" cy="12" r="1.8" fill="currentColor"/><circle cx="19" cy="12" r="1.8" fill="currentColor"/></svg>;
function FolderName({f,editing,done}:{f:Folder;editing:boolean;done(name:string|null):void}) {
  const settled=useRef(false);
  useEffect(()=>{if(editing)settled.current=false;},[editing]);
  const finish=(name:string|null)=>{if(settled.current)return;settled.current=true;done(name);};
  const [length,setLength]=useState(f.name.length),tooLong=length>FOLDER_NAME_MAX,errorId=useId();
  return editing ? <span className="pj-folder-edit"><input className="text pj-folder-input" autoFocus defaultValue={f.name} aria-label="Folder name" aria-invalid={tooLong} aria-describedby={tooLong?errorId:undefined}
    onFocus={e=>e.currentTarget.select()} onClick={e=>e.stopPropagation()} onChange={e=>setLength(e.currentTarget.value.trim().length)}
    onBlur={e=>finish(tooLong?null:e.currentTarget.value)} onKeyDown={e=>{e.stopPropagation();if(e.key==='Enter'){if(!tooLong)finish(e.currentTarget.value);}else if(e.key==='Escape')finish(null);}}/>
    {tooLong && <span className="control-error" id={errorId} role="status">Keep the name to {FOLDER_NAME_MAX} characters (now {length}).</span>}</span> : <span className="pj-name">{f.name}</span>;
}
function Star({on}:{on:boolean}) {
  return <svg viewBox="0 0 24 24" width="16" height="16" aria-hidden><path d="m12 3.5 2.6 5.3 5.9.9-4.3 4.1 1 5.8L12 16.9l-5.2 2.7 1-5.8-4.3-4.1 5.9-.9L12 3.5Z" fill={on?'currentColor':'none'} stroke="currentColor" strokeWidth="1.6" strokeLinejoin="round"/></svg>;
}

function Item({p,view,trash,current,selected,selecting,focused,favorite,canFavorite,renaming,blocked,folder,setRenaming,select,onMenu,onFavorite,onRestore,onDrag,onDragEnd}: {
  p:ProjectMeta;view:View;trash:boolean;current:boolean;selected:boolean;selecting:boolean;focused:boolean;favorite:boolean;canFavorite:boolean;renaming:boolean;blocked:boolean;folder?:string;
  setRenaming(on:boolean):void;select(event:SelectionEvent):void;onMenu(at:{x:number;y:number}):void;onFavorite():void;onRestore():void;onDrag(e:DragEvent):void;onDragEnd():void;
}) {
  const committed=useRef(false),[renameError,setRenameError]=useState<string|null>(null),[renameSaving,setRenameSaving]=useState(false);
  const commit=async(name:string)=>{if(committed.current)return;if(!name.trim()){setRenameError('Please enter a name.');return;}if(name.trim()===p.name){setRenaming(false);return;}committed.current=true;setRenameSaving(true);try{await (await import('../flow/project')).renameProject(p.id,name.trim());setRenameError(null);setRenaming(false);}catch(error){setRenameError(message(error));committed.current=false;}finally{setRenameSaving(false);}};
  useEffect(()=>{if(renaming)committed.current=false;},[renaming]);
  const click=(e:MouseEvent<HTMLElement>)=>{
    if(renaming || (e.target as Element).closest('button,input,.pj-check'))return;
    e.currentTarget.focus();
    if(trash)return;
    if(e.metaKey||e.ctrlKey||e.shiftKey||selecting)select({type:'CLICK',key:p.id,toggle:!e.shiftKey,shift:e.shiftKey});
    else void openProject(p.id);
  };
  const more=(e:MouseEvent<HTMLButtonElement>)=>{const r=e.currentTarget.getBoundingClientRect();onMenu({x:r.left,y:r.bottom+2});};
  const name=renaming
    ? <input className="text pj-rename" disabled={renameSaving || blocked} aria-busy={renameSaving} autoFocus defaultValue={p.name} aria-label="Project name" onClick={e=>e.stopPropagation()} onFocus={e=>e.currentTarget.select()} onBlur={e=>commit(e.currentTarget.value)} onKeyDown={e=>{e.stopPropagation();if(e.key==='Enter')commit(e.currentTarget.value);else if(e.key==='Escape'){committed.current=true;setRenaming(false);}}}/>
    : <span className="pj-name">{p.name}</span>;
  const check=!trash && <span className="pj-check"><Check checked={selected} aria-label={`Select ${p.name}`} onCheckedChange={()=>select({type:'CLICK',key:p.id,toggle:true})}/></span>;
  const actions=trash
    ? <button type="button" className="btn pj-restore" disabled={blocked} onClick={onRestore}>Restore</button>
    : <>
      <button type="button" className={`icon-btn pj-icon pj-fav${favorite?' on':''}`} aria-label="Favorite" aria-pressed={favorite} disabled={!canFavorite} onClick={onFavorite}><Star on={favorite}/></button>
      <button type="button" className="icon-btn pj-icon" aria-label={`Actions for ${p.name}`} aria-haspopup="menu" onClick={more}><Dots/></button>
    </>;
  const time=<time dateTime={p.savedAt?new Date(p.savedAt).toISOString():undefined}>{edited(p.savedAt)}</time>;
  const details=<span className="pj-sub">{current && <span className="pj-open">Open</span>}{folder!==undefined && <span>{folder}</span>}<span className="pj-count">{objects(p.objects)}</span>{time}</span>;
  return <div className={`pj-item${trash?' trash':''}${current?' current':''}${selected?' selected':''}${selecting?' selecting':''}`} role="row" tabIndex={focused?0:-1} data-id={p.id} aria-label={p.name} aria-selected={trash?undefined:selected} aria-current={current || undefined}
    draggable={!trash && !renaming} onDragStart={onDrag} onDragEnd={onDragEnd} onClick={click} onContextMenu={e=>{if(trash || renaming)return;e.preventDefault();if(!selected)select({type:'CLICK',key:p.id});onMenu({x:e.clientX,y:e.clientY});}}>
    {view==='grid' ? <>
      <div className="pj-thumb" role="gridcell"><Thumb p={p}/>{check}<span className="pj-overlay" onClick={e=>e.stopPropagation()}>{actions}</span></div>
      <div className="pj-meta" role="gridcell">{name}{renameError ? <span className="pj-rename-error" role="alert">{renameError}</span> : details}</div>
    </> : <>
      <div className="pj-cell pj-lead" role="gridcell">{check}<span className="pj-mini"><Thumb p={p}/></span></div>
      <div className="pj-cell pj-title" role="gridcell">{name}{renameError && <span className="pj-rename-error" role="alert">{renameError}</span>}{(current || folder!==undefined) && <span className="pj-sub">{current && <span className="pj-open">Open</span>}{folder!==undefined && <span>{folder}</span>}</span>}</div>
      <div className="pj-cell pj-col-objects" role="gridcell">{objects(p.objects)}</div>
      <div className="pj-cell pj-col-edited" role="gridcell">{time}</div>
      <div className="pj-cell pj-row-actions" role="gridcell" onClick={e=>e.stopPropagation()}>{actions}</div>
    </>}
  </div>;
}

class Boundary extends Component<{children:ReactNode},{failed:boolean}> {
  state={failed:false};static getDerivedStateFromError(){return {failed:true};}
  componentDidCatch(error:unknown){console.warn('Projects',error);}
  render(){return this.state.failed ? <div className="projects"><div className="pj-empty" role="alert"><strong>Projects unavailable</strong><button type="button" className="btn" onClick={()=>{refreshProjects();this.setState({failed:false});}}>Retry</button></div></div> : this.props.children;}
}
function ProjectsSkeleton({ view, home }: { view: string; home: boolean }) {
  const count = bootProjects(), rows = Array.from({ length: Math.min(count ?? Infinity, view === 'list' ? 8 : 12) }, (_, i) => i);
  const folders = home ? [...bootFolders()].sort((a, b) => a.localeCompare(b, undefined, { numeric: true, sensitivity: 'base' })) : [];
  return <div className="pj-scroll" role="status" aria-busy="true" aria-label="Loading projects">
    {folders.length > 0 && <div className="pj-tiles" aria-hidden="true">{folders.map((name, i) => <div key={i} className="pj-tile"><button type="button" className="pj-tile-open" disabled tabIndex={-1}><span className="pj-tile-text">
      <HeldText className="pj-name" text={name}/><HeldText className="pj-sub" text="0 projects"/></span></button><button type="button" className="icon-btn pj-icon" disabled tabIndex={-1}/></div>)}</div>}
    {home && rows.length > 0 && <div className="pj-section"><h2>Recent</h2><button type="button" className="link" disabled tabIndex={-1}>Show all</button></div>}
    {view === 'list'
      ? <div className="pj-list">{rows.map((i) => <div key={i} className="pj-item pj-skel"><span className="pj-cell"><span className="pj-mini"><Skeleton block width="100%" height="100%"/></span></span>
          <span className="pj-cell pj-title"><Skeleton width="50%" height={12}/></span><span className="pj-cell"><Skeleton width={48} height={10}/></span><span className="pj-cell"><Skeleton width={72} height={10}/></span><span/></div>)}</div>
      : <div className="pj-grid">{rows.map((i) => <div key={i} className="pj-item pj-skel"><div className="pj-thumb"><Skeleton block width="100%" height="100%"/></div>
          <div className="pj-meta"><span className="pj-name"><Skeleton width="60%" height={12} style={{ margin: '3px 0' }}/></span><span className="pj-sub"><Skeleton width="45%" height={10} style={{ margin: '3px 0' }}/></span></div></div>)}</div>}
  </div>;
}

export default function ProjectsView(){return <Boundary><Projects/></Boundary>;}

function Projects() {
  const list=useProjects(),current=useProject(s=>s.context.id),status=useSyncExternalStore(subscribeProjectList,projectListSnapshot);
  const [storage,setStorage]=useState<ProjectStore|null>(null);
  useEffect(()=>{let active=true;void projectStore().then(store=>{if(active)setStorage(store);});return()=>{active=false;};},[status.state==='ready']);
  const commands=useSyncExternalStore(storage ? storage.subscribeCommands : noSubscribe,storage ? storage.commands : emptyCommands);
  const blocked=(id:string)=>commands.some(c=>c.id===id);
  usePreference('projects.favorites');usePreference('projects.view');usePreference('projects.sort');
  const favoriteIds=preferenceJson<unknown>('projects.favorites',[]),favorites=new Set(Array.isArray(favoriteIds)?favoriteIds.filter((id):id is string=>typeof id==='string'):[]),favoriteKey=[...favorites].join('\n');
  const view:View=preferenceText('projects.view')==='list'?'list':'grid';
  const sortPref=preferenceText('projects.sort') ?? 'edited',sort:Sort=Object.hasOwn(sorters,sortPref)?sortPref as Sort:'edited';
  const prefsLoaded=useSyncExternalStore(subscribeSavedPreferences,savedPreferencesSnapshot).hydrated;
  const toggleFavorite=(id:string)=>{if(!prefsLoaded)return;const next=new Set(favorites);if(!next.delete(id))next.add(id);savePreferences({'projects.favorites':[...next]});};
  const folderState=useFolders(),folders=folderState.folders;
  const [place,setPlaceState]=useState<Place>(lastPlace);
  const [railOpen,setRailOpen]=useState(false),closeRail=()=>setRailOpen(false);
  const phone=usePhoneSheet(),[slot,setSlot]=useState<HTMLElement|null>(null);
  useEffect(()=>setSlot(document.getElementById('topbar-slot')),[phone]);
  const barActions=<>
    <div className="seg pj-view" role="group" aria-label="View">
      <button type="button" aria-pressed={view==='grid'} aria-label="Grid" onClick={()=>savePreferences({'projects.view':'grid'})}><svg viewBox="0 0 16 16" width="14" height="14" aria-hidden><path d="M2 2h5v5H2zM9 2h5v5H9zM2 9h5v5H2zM9 9h5v5H9z" fill="currentColor"/></svg></button>
      <button type="button" aria-pressed={view==='list'} aria-label="List" onClick={()=>savePreferences({'projects.view':'list'})}><svg viewBox="0 0 16 16" width="14" height="14" aria-hidden><path d="M2 3h12v2H2zM2 7h12v2H2zM2 11h12v2H2z" fill="currentColor"/></svg></button>
    </div>
    <button type="button" className="btn" onClick={fileVerbs.import}>Import</button>
    <button type="button" className="btn-primary" onClick={fileVerbs.new}>New project</button>
  </>;
  const go=(next:Place)=>{lastPlace=next;setPlaceState(next);setRenaming(null);setMenu(null);setProjectsQuery('');closeRail();};
  const inTrash=place.kind==='trash',placeFolder=place.kind==='folder'?folders.find(f=>f.id===place.id):undefined;
  useEffect(()=>{if(place.kind==='folder' && folderState.state==='ready' && !placeFolder)go({kind:'home'});},[place,folderState.state,placeFolder]);
  const [,redraw]=useState(0);
  const [trash,setTrash]=useState<ProjectMeta[]|null>(null),[trashError,setTrashError]=useState<string|null>(null);
  const loadTrash=()=>listTrash().then(rows=>{setTrash(rows);setTrashError(null);},error=>setTrashError(message(error)));
  useEffect(()=>{void loadTrash();},[list]);
  const query=useProjectsQuery(),q=query.trim().toLocaleLowerCase();
  const known=useMemo(()=>new Set(folders.map(f=>f.id)),[folders]);
  const folderOf=(p:ProjectMeta)=>p.folderId && known.has(p.folderId)?p.folderId:null;
  const counts=useMemo(()=>{
    const now=Date.now(),all=list??[],inFolder=new Map<string,number>();
    for(const p of all){const f=folderOf(p);if(f)inFolder.set(f,(inFolder.get(f)??0)+1);}
    return {recent:all.filter(p=>now-p.savedAt<RECENT_MS).length,favorites:all.filter(p=>favorites.has(p.id)).length,unsorted:all.filter(p=>!folderOf(p)).length,trash:trash?.length,inFolder} as const;
  },[list,trash,favoriteKey,known]);
  const searching=!!q && !inTrash;
  const shown=useMemo(()=>{
    const now=Date.now(),match=(p:ProjectMeta)=>!q || p.name.toLocaleLowerCase().includes(q);
    if(inTrash)return (trash??[]).filter(match).sort(sorters[sort]);
    const all=list??[];
    if(searching)return all.filter(match).sort(sorters[sort]);
    if(place.kind==='home')return [...all].sort(sorters.edited).slice(0,HOME_RECENT);
    return all.filter(p=>place.kind==='recent'?now-p.savedAt<RECENT_MS:place.kind==='favorites'?favorites.has(p.id):place.kind==='unsorted'?!folderOf(p):place.kind==='folder' && folderOf(p)===place.id).sort(sorters[sort]);
  },[list,trash,place,inTrash,searching,q,sort,favoriteKey,known]);
  const subfolders=searching?folders.filter(f=>f.name.toLocaleLowerCase().includes(q)):place.kind==='home'?childFolders(folders,null):placeFolder?childFolders(folders,placeFolder.id):[];
  const folderName=(p:ProjectMeta)=>{const f=folderOf(p);return f?folders.find(x=>x.id===f)!.name:'Unsorted';};
  const visible=useMemo(()=>inTrash?[]:shown.map(p=>p.id),[shown,inTrash]);
  const [raw,setSelection]=useState(()=>createSelection('projects',visible)),selection=useMemo(()=>reduceSelection(raw,{type:'SYNC',scope:'projects',visible}),[raw,visible]);
  const latest=useRef(selection);latest.current=selection;
  const selected=selectedVisible(selection),selectedSet=useMemo(()=>new Set(selected),[selection]);
  const [renaming,setRenaming]=useState<string|null>(null),[error,setError]=useState<string|null>(null);
  const ops=useMachine(projectsOps),busy=ops.value==='deleting',retrying=ops.value==='retrying'?ops.context.id:null,writing=ops.value!=='idle';
  const [menu,setMenu]=useState<{id:string;x:number;y:number}|null>(null),grid=useRef<HTMLDivElement>(null);
  const [trashFocus,setTrashFocus]=useState<string|null>(null);
  const focusKey=inTrash?(shown.some(p=>p.id===trashFocus)?trashFocus:shown[0]?.id):selection.focus??visible[0];
  const dispatch=(event:SelectionEvent,focus=false)=>{const next=reduceSelection(latest.current,event);latest.current=next;setSelection(next);if(focus && next.focus)requestAnimationFrame(()=>{const row=[...(grid.current?.querySelectorAll<HTMLElement>('[data-id]')??[])].find(el=>el.dataset.id===next.focus);row?.focus();row?.scrollIntoView({block:'nearest'});});};
  const restoreEntries=async(entries:readonly ProjectUndo[])=>{
    let restored=0;const failed:string[]=[];
    try{
      for(const entry of entries){
        if(projectUndos.get(entry.id)!==entry)continue;
        try{await entry.run();if(projectUndos.get(entry.id)===entry)projectUndos.delete(entry.id);restored++;}
        catch(error){failed.push(`${entry.name}: ${message(error)}`);}
      }
      setError(failed.length?`${restored} restored, ${failed.length} not confirmed (${failed.join('; ')}). Undo stays available.`:null);
      if(restored)toast('restored_2', { restored, value: restored===1?'project':'projects' });
    }finally{void loadTrash();}
  };
  const restore=(entries:readonly ProjectUndo[])=>runProjectsOp('restoring',()=>restoreEntries(entries));
  const restoreFromTrash=async(p:ProjectMeta)=>{
    const undo=projectUndos.get(p.id);
    if(undo){await restore([undo]);return;}
    await runProjectsOp('retrying',async()=>{
      if(!trashRestores.has(p.id))trashRestores.set(p.id,p.revision);
      setError(null);
      try{await (await import('../flow/project')).restoreArchivedProject(p.id,trashRestores.get(p.id));trashRestores.delete(p.id);toast('restored_3', { name: p.name });}
      catch(error){setError(`Couldn't finish restoring: ${message(error)}`);}
      finally{void loadTrash();}
    },p.id);
  };
  const retry=(command:Readonly<ProjectCommandStatus>)=>runProjectsOp('retrying',async()=>{
    setError(null);
    try{
      const flow=await import('../flow/project');
      if(command.kind==='rename'){await flow.renameProject(command.id,command.name!);setRenaming(id=>id===command.id?null:id);}
      else if(command.kind==='duplicate')await flow.duplicateProject(command.id);
      else if(command.kind==='archive'){
        const run=await flow.deleteProject(command.id);projectUndos.set(command.id,{id:command.id,name:list?.find(p=>p.id===command.id)?.name ?? command.id,run});
      }else if(projectUndos.has(command.id))await restoreEntries([projectUndos.get(command.id)!]);
      else if(trashRestores.has(command.id)){await flow.restoreArchivedProject(command.id,trashRestores.get(command.id));trashRestores.delete(command.id);}
      else throw new Error('Please try undoing again from the original notification.');
    }catch(error){setError(message(error));}
    finally{void loadTrash();}
  },command.operationId);
  const dismiss=(command:Readonly<ProjectCommandStatus>)=>runProjectsOp('retrying',async()=>{
    const undo=projectUndos.get(command.id);
    try{
      await (await import('../flow/project')).discardRejectedProjectCommand(command.operationId);
      if(command.kind==='restore' && projectUndos.get(command.id)===undo)projectUndos.delete(command.id);
      if(command.kind==='restore')trashRestores.delete(command.id);
      setError(null);
      await refreshProjects();
    }catch(error){setError(message(error));}
  },command.operationId);
  const remove=async(ids:readonly string[],ask=true)=>{
    const targets=(list??[]).filter(p=>ids.includes(p.id));if(projectsOps.get().value!=='idle' || !targets.length || targets.some(p=>blocked(p.id)))return;
    const unsaved=targets.find(p=>openWithUnsaved(p.id));
    if(unsaved && !await confirmDialog('Discard your unsaved changes?',`Heads up! ${unsaved.name} is open with unsaved changes — moving it to Trash will discard them.`,['Discard and move to Trash','Cancel'],true))return;
    if(ask && !unsaved && !await confirmDialog(targets.length===1?'Move to Trash?':`Move ${targets.length} projects to Trash?`,names(targets),['Move to Trash','Cancel'],true))return;
    await runProjectsOp('deleting',async()=>{
    setError(null);const undo:ProjectUndo[]=[],failed:string[]=[];
    for(const p of targets){const run=await deleteProject(p.id);if(run){const entry={id:p.id,name:p.name,run};undo.push(entry);projectUndos.set(p.id,entry);}else failed.push(p.name);}
      setSelection(s=>reduceSelection(s,{type:'SYNC',scope:'projects',visible:visible.filter(id=>!undo.some(p=>p.id===id))}));
      if(failed.length && undo.length)setError(`Moved ${undo.length} to Trash, but ${failed.length} couldn't be moved: ${failed.join(', ')}.`);
      if(undo.length)toast('moved_trash', { value: undo.length===1?undo[0].name:`${undo.length} projects` }, { action: {label:'Undo',run:()=>void restore(undo)} });
    });
  };
  const onKey=(e:KeyboardEvent)=>{
    const command=e.ctrlKey||e.metaKey,focus=(e.target as Element).closest<HTMLElement>('.pj-item[data-id]')?.dataset.id ?? focusKey;
    if((e.target as Element).closest('input,textarea,[contenteditable=true]') || busy)return;
    if((e.target as Element).closest('button') && !(command&&e.key.toLowerCase()==='a') && e.key!=='Escape')return;
    if(inTrash){
      if(['ArrowLeft','ArrowRight','ArrowUp','ArrowDown','Home','End'].includes(e.key)){
        const rows=[...(grid.current?.querySelectorAll<HTMLElement>('[data-id]')??[])],at=rows.findIndex(r=>r.dataset.id===focus);
        const next=e.key==='Home'?0:e.key==='End'?rows.length-1:Math.max(0,Math.min(rows.length-1,at+(['ArrowLeft','ArrowUp'].includes(e.key)?-1:1)));
        const row=rows[next];if(row?.dataset.id){setTrashFocus(row.dataset.id);row.focus();row.scrollIntoView({block:'nearest'});}
      }else return;
      e.preventDefault();e.stopPropagation();return;
    }
    if(command && e.key.toLowerCase()==='a')dispatch({type:'SELECT_ALL'});
    else if(e.key==='Escape')dispatch({type:'ESCAPE'});
    else if(['ArrowLeft','ArrowRight','ArrowUp','ArrowDown','Home','End'].includes(e.key)){
      const items=[...(grid.current?.querySelectorAll<HTMLElement>('.pj-item')??[])],cols=Math.max(1,items.filter(c=>c.offsetTop===items[0]?.offsetTop).length);
      const direction=e.key==='Home'?'first':e.key==='End'?'last':['ArrowLeft','ArrowUp'].includes(e.key)?'previous':'next',steps=e.key==='ArrowUp'||e.key==='ArrowDown'?cols:1;
      for(let i=0;i<steps;i++)dispatch({type:'ARROW',direction,shift:e.shiftKey,focusOnly:command},i===steps-1);
    }else if(e.key==='Enter' && focus)void openProject(focus);
    else if(e.key===' ' && focus)dispatch({type:'CLICK',key:focus,toggle:true,shift:e.shiftKey});
    else if(e.key==='F2' && selected.length===1 && !blocked(selected[0]))setRenaming(selected[0]);
    else if(e.key==='Delete' || e.key==='Backspace')void remove(selected.length?selected:focus?[focus]:[]);
    else if((e.key==='ContextMenu'||e.shiftKey&&e.key==='F10') && focus){const row=[...(grid.current?.querySelectorAll<HTMLElement>('[data-id]')??[])].find(el=>el.dataset.id===focus);if(row){const r=row.getBoundingClientRect();setMenu({id:focus,x:r.left+8,y:r.bottom-8});}}
    else return;e.preventDefault();e.stopPropagation();
  };
  const onDrop=(e:DragEvent)=>{if(!e.dataTransfer.files.length)return;e.preventDefault();void newProjectFrom(e.dataTransfer.files);};
  const [dragging,setDragging]=useState<'projects'|'folder'|null>(null),[editing,setEditing]=useState<string|null>(null);
  const dragIds=(id:string)=>selected.includes(id)?selected:[id];
  const ghost=(e:DragEvent,text:string)=>{
    const el=document.createElement('div');el.className='pj-ghost';el.textContent=text;document.body.append(el);
    e.dataTransfer.setDragImage(el,14,14);setTimeout(()=>el.remove());
  };
  const onDrag=(id:string)=>(e:DragEvent)=>{
    const ids=dragIds(id);e.dataTransfer.setData(DRAG,JSON.stringify(ids));e.dataTransfer.effectAllowed='move';
    ghost(e,ids.length===1?(list?.find(p=>p.id===id)?.name ?? 'Project'):`${ids.length} projects`);setDragging('projects');
  };
  const onFolderDrag=(f:Folder)=>(e:DragEvent)=>{e.stopPropagation();e.dataTransfer.setData(FOLDER_DRAG,f.id);e.dataTransfer.effectAllowed='move';ghost(e,f.name);setDragging('folder');};
  const endDrag=()=>{setDragging(null);document.querySelectorAll('.projects .drop').forEach(el=>el.classList.remove('drop'));};
  const move=async(ids:readonly string[],folder:string|null)=>{
    if(!ids.length)return;
    const name=folder?folders.find(f=>f.id===folder)?.name ?? 'folder':'Unsorted';
    try{await moveProjects(ids,folder);toast('moved', { value: ids.length===1?(list?.find(p=>p.id===ids[0])?.name ?? 'Project'):`${ids.length} projects`, name });if(ids.length>1)dispatch({type:'CLEAR'});}
    catch(error){setError(`Oops! Couldn't move item: ${message(error)}`);}
  };
  const uniqueName=(parent:string|null)=>{const taken=new Set(childFolders(folders,parent).map(f=>f.name.toLocaleLowerCase()));let name='New folder';for(let n=2;taken.has(name.toLocaleLowerCase());n++)name=`New folder ${n}`;return name;};
  const newFolder=async(parent:string|null,ids?:readonly string[])=>{
    try{const id=await createFolder(uniqueName(parent),parent);if(parent)collapsed.delete(parent);if(ids?.length)await moveProjects(ids,id);setEditing(id);}
    catch(error){setError(`Oops! Couldn't create folder: ${message(error)}`);}
  };
  const named=(f:Folder)=>async(name:string|null)=>{
    setEditing(null);const next=name?.trim();if(!next || next===f.name)return;
    try{await renameFolder(f.id,next);setError(null);}catch(error){setError(`Oops! Couldn't rename: ${message(error)}`);}
  };
  const nest=async(id:string,parent:string|null)=>{
    const f=folders.find(x=>x.id===id);if(!f || (f.parentId ?? null)===parent || id===parent)return;
    try{await moveFolder(id,parent);if(parent)collapsed.delete(parent);}catch(error){setError(`Oops! Couldn't move item: ${message(error)}`);}
  };
  type Target={folder:string|null}|'trash'|'new';
  const dropTarget=(target:Target,folders=true)=>({
    onDragOver:(e:DragEvent<HTMLElement>)=>{
      const projects=e.dataTransfer.types.includes(DRAG),folder=folders && e.dataTransfer.types.includes(FOLDER_DRAG) && typeof target==='object';
      if(!projects && !folder)return;e.preventDefault();e.stopPropagation();e.dataTransfer.dropEffect='move';e.currentTarget.classList.add('drop');
    },
    onDragLeave:(e:DragEvent<HTMLElement>)=>{if(!e.currentTarget.contains(e.relatedTarget as Node))e.currentTarget.classList.remove('drop');},
    onDrop:(e:DragEvent<HTMLElement>)=>{
      e.currentTarget.classList.remove('drop');
      const folder=e.dataTransfer.getData(FOLDER_DRAG),raw=e.dataTransfer.getData(DRAG);
      if(folder && typeof target==='object'){e.preventDefault();e.stopPropagation();endDrag();void nest(folder,target.folder);return;}
      if(!raw)return;e.preventDefault();e.stopPropagation();endDrag();
      let parsed:unknown;try{parsed=JSON.parse(raw);}catch{return;}
      const ids=Array.isArray(parsed)?parsed.filter((id):id is string=>typeof id==='string'):[];
      if(target==='trash')void remove(ids,false);
      else if(target==='new')void newFolder(placeFolder?.id ?? null,ids);
      else void move(ids,target.folder);
    },
  });
  const removeFolder=async(f:Folder)=>{
    const n=counts.inFolder.get(f.id) ?? 0,inner=childFolders(folders,f.id).length;
    const detail=[`${count(n,'project moves','projects move')} to Unsorted.`,inner?`${count(inner,'folder moves','folders move')} up a level.`:''].filter(Boolean).join(' ');
    if(!await confirmDialog(`Delete “${f.name}”?`,detail,['Delete folder','Cancel'],true))return;
    try{await deleteFolder(f.id);if(place.kind==='folder' && place.id===f.id)go(f.parentId?{kind:'folder',id:f.parentId}:{kind:'home'});toast('deleted', { name: f.name });}
    catch(error){setError(`Oops! Couldn't delete: ${message(error)}`);}
  };
  const moveEntries=(ids:readonly string[]):MenuEntry[]=>{
    const at=(id:string)=>{const p=list?.find(x=>x.id===id);return p?folderOf(p):null;};
    return [
      {label:'Unsorted',disabled:ids.every(id=>at(id)===null),action:()=>void move(ids,null)},
      ...folderTree(folders).map(({folder,depth}):MenuEntry=>({label:`${' '.repeat(depth)}${folder.name}`,disabled:ids.every(id=>at(id)===folder.id),action:()=>void move(ids,folder.id)})),
      'sep',
      {label:'New folder',action:()=>void newFolder(null,ids)},
    ];
  };
  const [folderMenu,setFolderMenu]=useState<{id:string;x:number;y:number}|null>(null);
  const folderMenuAt=(f:Folder)=>(e:MouseEvent<HTMLElement>)=>{e.preventDefault();e.stopPropagation();const r=e.currentTarget.getBoundingClientRect();setFolderMenu({id:f.id,...(e.type==='contextmenu'?{x:e.clientX,y:e.clientY}:{x:r.left,y:r.bottom+2})});};
  const folderItems=(f:Folder):MenuEntry[]=>[
    {label:'Open',action:()=>go({kind:'folder',id:f.id})},
    {label:'New folder inside',action:()=>void newFolder(f.id)},
    {label:'Rename',action:()=>setEditing(f.id)},
    'sep',
    {label:'Delete folder…',action:()=>void removeFolder(f)},
  ];
  const menuProject=menu && list?.find(p=>p.id===menu.id);
  const menuItems=(p:ProjectMeta):MenuEntry[]=>[
    {label:'Open',action:()=>void openProject(p.id)},
    {label:'Rename',shortcut:'F2',disabled:blocked(p.id),action:()=>setRenaming(p.id)},
    {label:'Duplicate',disabled:blocked(p.id),action:()=>void duplicateProject(p.id)},
    {label:'Move to',sub:moveEntries(dragIds(p.id))},
    {label:favorites.has(p.id)?'Remove from favorites':'Add to favorites',disabled:!prefsLoaded,action:()=>toggleFavorite(p.id)},
    {label:'Export 3MF…',output:true,action:()=>void exportProject(p)},
    'sep',
    {label:'Move to Trash',shortcut:'Del',disabled:busy || blocked(p.id),action:()=>void remove(selected.length>1 && selected.includes(p.id)?selected:[p.id])},
  ];
  const crumbs=placeFolder?folderPath(folders,placeFolder.id):[];
  const title=searching?'Search':placeFolder?placeFolder.name:inTrash?'Trash':PLACES.find(x=>x.kind===place.kind)?.label ?? 'Home';
  const loading=inTrash?!trash && !trashError:!list || place.kind==='home' && folderState.state==='loading';
  const failed=inTrash?trashError:status.state==='failed'?status.error:folderState.state==='failed'?folderState.error:null;
  const empty=inTrash?'Trash is empty':place.kind==='favorites'?'No favorites':place.kind==='recent'?'Nothing edited this week':place.kind==='unsorted'?'Nothing unsorted':placeFolder?'Drag projects here':'No projects yet';
  const item=(p:ProjectMeta)=><Item key={p.id} p={p} view={view} trash={inTrash} current={!inTrash && p.id===current} selected={selectedSet.has(p.id)} selecting={selected.length>0} focused={p.id===focusKey} favorite={favorites.has(p.id)} canFavorite={prefsLoaded} renaming={renaming===p.id} blocked={blocked(p.id) || retrying===p.id}
    folder={searching?folderName(p):undefined} setRenaming={on=>{if(!on || !blocked(p.id))setRenaming(on?p.id:null);}} select={event=>dispatch(event)} onMenu={at=>setMenu({id:p.id,...at})} onFavorite={()=>toggleFavorite(p.id)} onRestore={()=>void restoreFromTrash(p)} onDrag={onDrag(p.id)} onDragEnd={endDrag}/>;
  const head=view==='list' && <div className="pj-head" role="row"><span role="columnheader" className="pj-lead"/><span role="columnheader">Name</span><span role="columnheader" className="pj-col-objects">Objects</span><span role="columnheader" className="pj-col-edited">Edited</span><span role="columnheader" className="pj-row-actions"/></div>;
  const railItem=(key:string,label:string,on:boolean,n:number|undefined,open:()=>void,target?:Target)=>
    <button key={key} type="button" className={`pj-filter${on?' on':''}`} aria-current={on?'page':undefined} onClick={open} {...(target?dropTarget(target):{})}>
      <span>{label}</span>{n!==undefined && <span className="pj-n">{n}</span>}
    </button>;
  const tileEditing=!inTrash && (place.kind==='home' || !!placeFolder || searching) && subfolders.some(f=>f.id===editing);
  const tree=folderTree(folders).filter(({folder})=>!folderPath(folders,folder.parentId ?? null).some(a=>collapsed.has(a.id)));
  const tiles=!inTrash && (place.kind==='home' && subfolders.length>0 || placeFolder || searching && subfolders.length>0) && <div className="pj-tiles" role="list" aria-label="Folders">
    {subfolders.map(f=><div key={f.id} role="listitem" className="pj-tile" draggable={editing!==f.id} onDragStart={onFolderDrag(f)} onDragEnd={endDrag} {...dropTarget({folder:f.id})} onContextMenu={folderMenuAt(f)}>
      <button type="button" className="pj-tile-open" onClick={()=>{if(editing!==f.id)go({kind:'folder',id:f.id});}}><span className="pj-tile-text"><FolderName f={f} editing={editing===f.id} done={named(f)}/><span className="pj-sub">{count(counts.inFolder.get(f.id) ?? 0,'project','projects')}</span></span></button>
      <button type="button" className="icon-btn pj-icon" aria-label={`Actions for ${f.name}`} aria-haspopup="menu" onClick={folderMenuAt(f)}><Dots/></button>
    </div>)}
    {!searching && placeFolder && <div role="listitem" className="pj-tile pj-tile-new" {...dropTarget('new',false)}>
      <button type="button" className="pj-tile-open" onClick={()=>void newFolder(placeFolder?.id ?? null)}><span className="pj-tile-text"><span className="pj-name">New folder</span></span></button>
    </div>}
  </div>;
  const grid_=shown.length>0 && <div ref={grid} className={`pj-${view}`} role="grid" aria-label={title} aria-multiselectable={inTrash?undefined:true} aria-busy={busy || status.state==='loading'} onKeyDown={onKey}>{head}{shown.map(item)}</div>;
  return <div className={`projects${dragging?` dragging-${dragging}`:''}`} onDragOver={e=>{if(e.dataTransfer.types.includes('Files'))e.preventDefault();}} onDrop={onDrop}><div className="pj-layout">
    {railOpen && <div className="pj-scrim" aria-hidden onClick={closeRail}/>}
    <nav className="pj-side" id="pj-rail" aria-label="Projects" data-open={railOpen} onKeyDown={e=>{if(railOpen && e.key==='Escape'){e.stopPropagation();closeRail();}}}>
      {PLACES.filter(x=>x.kind!=='recent').map(x=>railItem(x.kind,x.label,place.kind===x.kind && !searching,x.kind==='home'?undefined:counts[x.kind],()=>go({kind:x.kind}),x.kind==='unsorted'?{folder:null}:x.kind==='home'?{folder:null}:undefined))}
      <div className="pj-tree" role="tree" aria-label="Folders">
        {tree.map(({folder:f,depth})=>{const has=childFolders(folders,f.id).length>0,open=!collapsed.has(f.id),on=place.kind==='folder' && place.id===f.id && !searching;
          return <div key={f.id} className="pj-node" role="treeitem" aria-level={depth+1} aria-expanded={has?open:undefined} aria-selected={on} style={{paddingLeft:`${depth*12}px`}}
            draggable={editing!==f.id} onDragStart={onFolderDrag(f)} onDragEnd={endDrag} {...dropTarget({folder:f.id})} onContextMenu={folderMenuAt(f)}>
            {has ? <button type="button" className={`pj-caret${open?' open':''}`} aria-label={open?`Collapse ${f.name}`:`Expand ${f.name}`} onClick={()=>{if(!collapsed.delete(f.id))collapsed.add(f.id);redraw(n=>n+1);}}><svg viewBox="0 0 16 16" width="12" height="12" aria-hidden><path d="m6 4 4 4-4 4" fill="none" stroke="currentColor" strokeWidth="1.6"/></svg></button> : <span className="pj-caret" aria-hidden/>}
            {editing===f.id && !tileEditing ? <FolderName f={f} editing done={named(f)}/>
              : <button type="button" className={`pj-filter${on?' on':''}`} aria-current={on?'page':undefined} onClick={()=>go({kind:'folder',id:f.id})}><span>{f.name}</span><span className="pj-n">{counts.inFolder.get(f.id) ?? 0}</span></button>}
          </div>;})}
        <button type="button" className="pj-filter pj-add" {...dropTarget('new',false)} onClick={()=>void newFolder(null)}><span>+ New folder</span></button>
      </div>
      <div className="pj-side-end">{railItem('trash','Trash',inTrash,counts.trash,()=>go({kind:'trash'}),'trash')}</div>
    </nav>
    <section className="pj-main" aria-label={title}>
      <header className="pj-bar">
        <button type="button" className="btn pj-place" aria-expanded={railOpen} aria-controls="pj-rail" onClick={()=>setRailOpen(true)}><span>{title}</span><Caret/></button>
        {crumbs.length && !searching ? <nav className="pj-crumbs" aria-label="Folder path">
          <button type="button" className="btn-ghost" onClick={()=>go({kind:'home'})}>Home</button>
          {crumbs.map((f,i)=>i<crumbs.length-1
            ? <span key={f.id}><span aria-hidden>›</span><button type="button" className="btn-ghost" onClick={()=>go({kind:'folder',id:f.id})}>{f.name}</button></span>
            : <span key={f.id}><span aria-hidden>›</span><h1>{f.name}</h1></span>)}
        </nav> : <h1>{title}</h1>}
        <input className="text pj-search" type="search" placeholder="Search" aria-label="Search all projects" value={query} onChange={e=>setProjectsQuery(e.target.value)} onKeyDown={e=>{if(e.key==='Escape' && query){e.stopPropagation();setProjectsQuery('');}}}/>
        <div className="pj-tools">
          <Dropdown className="pj-sort" aria-label="Sort by" options={SORTS} value={sort} onValueChange={v=>savePreferences({'projects.sort':v})}/>
          {phone&&slot?createPortal(<div className="pj-top-actions">{barActions}</div>,slot):barActions}
        </div>
      </header>
      {failed && !loading && <div className="pj-line" role="alert"><span>{failed}</span><button type="button" className="btn" onClick={()=>{if(inTrash)void loadTrash();else void refreshProjects();}}>Retry</button></div>}
      {error && <div className="pj-line" role="alert"><span>{error}</span><button type="button" className="btn-ghost" onClick={()=>setError(null)}>Dismiss</button></div>}
      {commands.map(command=>{
        const pending=command.state==='preparing' || command.state==='sending',name=list?.find(p=>p.id===command.id)?.name ?? trash?.find(p=>p.id===command.id)?.name ?? command.name ?? 'Project';
        const state=pending?'Saving…':command.state==='acknowledged'?'Saved':command.held?command.error:command.state==='conflict'?`Changed elsewhere: ${command.error}`:`Not saved: ${command.error}`;
        return <div className={`pj-line${command.state==='failed'||command.state==='conflict'?' bad':''}`} key={command.operationId} aria-busy={pending} role="status">
          <span><strong>{commandVerb[command.kind]} {name}</strong><Sep/>{state}</span>
          {!pending && (command.kind!=='restore' || projectUndos.has(command.id) || trashRestores.has(command.id)) && <button type="button" className="btn" disabled={writing} onClick={()=>void retry(command)}>{command.state==='acknowledged'?'Finish':'Retry'}</button>}
          {(command.state==='conflict' || command.held) && <button type="button" className="btn-ghost" disabled={writing} onClick={()=>void dismiss(command)}>Dismiss</button>}
        </div>;
      })}
      {!!projectUndos.size && !inTrash && <div className="pj-line"><span>{projectUndos.size} moved to Trash</span><button type="button" className="btn" disabled={writing} onClick={()=>void restore([...projectUndos.values()])}>{ops.value==='restoring'?'Restoring…':'Undo'}</button></div>}
      {selected.length>0 && !inTrash && <div className="pj-line pj-selection">
        <strong>{selected.length} selected</strong>
        <button type="button" className="btn-ghost" disabled={busy || selected.some(blocked)} onClick={()=>void remove(selected)}>Move to Trash</button>
        <button type="button" className="btn-ghost pj-clear" onClick={()=>dispatch({type:'CLEAR'})}>Clear</button>
      </div>}
      {loading ? <ProjectsSkeleton view={view} home={place.kind==='home'}/>
        : <div className="pj-scroll scroll-fade">
          {tiles}
          {place.kind==='home' && !searching && shown.length>0 && <div className="pj-section"><h2>Recent</h2><button type="button" className="link" onClick={()=>go({kind:'recent'})}>Show all</button></div>}
          {grid_ || (!(tiles && !searching && place.kind==='home') && <div className="pj-empty">{failed ? null : q ? <><strong>No matches</strong><button type="button" className="btn" onClick={()=>setProjectsQuery('')}>Clear search</button></>
            : <><strong>{empty}</strong>{place.kind==='home' && <div className="pj-empty-actions"><button type="button" className="btn-primary" onClick={fileVerbs.new}>New project</button><button type="button" className="btn" onClick={fileVerbs.import}>Import model</button></div>}</>}</div>)}
        </div>}
    </section></div>
    {menu && menuProject && <ActionMenu label="Project actions" at={menu} onClose={()=>setMenu(null)} items={menuItems(menuProject)}/>}
    {folderMenu && folders.some(f=>f.id===folderMenu.id) && <ActionMenu label="Folder actions" at={folderMenu} onClose={()=>setFolderMenu(null)} items={folderItems(folders.find(f=>f.id===folderMenu.id)!)}/>}
  </div>;
}
