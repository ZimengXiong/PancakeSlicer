import type { Config, ConfigValue, PresetType } from '../api/types';
import { getState, markStale, resolvePreset, setState, type State } from '../state/store';
import { librarySync } from '../flow/library-sync';
import type { SettingMove } from '../flow/profile-save';
import { plateLabel, plateSetup } from './plate-setup';
import { MAX_FLUSH_VOLUME } from '../flush-state';
import { slotFilaments } from './slots';
import { createMachine, type Machine } from '../flow/fsm';
import { encodeLibraryWrite, putLibrary, readLibrary } from '../persist/profile-library';
import { holdReloadWhile } from '../build-update';
import { draftChanges, rebasedProfileDraft, removedInUseDrafts } from './shared-draft';
import { valueBelow } from './view';
import { profileSaveFor } from '../flow/profile-save';
import type { ProfileDraft } from './ProfileEditor';
import { configureLibraryWrites, knowLibrary, libraryWritesPending, LibraryWriteError, submitLibraryWrite,
  type EntityKey, type LibraryChange, type LibraryWriteIntent } from './library-writes';
import { getRecord, recordKey, sameRecord, setRecord, type AnyRecord, type FilamentRecord, type LibraryRecords, type PrinterRecord, type ProcessRecord, type RecordKind } from './records';
import { randomId } from '../random-id';

const conflict=(message:string)=>new LibraryWriteError(message,'revision_conflict');
const kindOf=(type:PresetType):RecordKind=>type==='machine'?'printer':type==='filament'?'filament':'process';

configureLibraryWrites({read:readLibrary,encode:encodeLibraryWrite,put:putLibrary,adopt:adoptLibrary,loading:()=>getState().profileLibraryLoad.state==='loading'});
holdReloadWhile(libraryWritesPending);

let adopting=false;
function settingChanges(a:AnyRecord|undefined,b:AnyRecord|undefined):[string,ConfigValue|undefined,ConfigValue|undefined][]{
  const out:[string,ConfigValue|undefined,ConfigValue|undefined][]=[];
  for(const field of ['settings','values','machine','printOverrides']){
    const x=((a as unknown as Record<string,Config|undefined>|undefined)?.[field])??{},y=((b as unknown as Record<string,Config|undefined>|undefined)?.[field])??{};
    for(const k of new Set([...Object.keys(x),...Object.keys(y)]))if(JSON.stringify(x[k])!==JSON.stringify(y[k]))out.push([k,x[k],y[k]]);
  }
  return out;
}
function printerMoves(s:State,library:LibraryRecords,a:PrinterRecord|undefined,b:PrinterRecord|undefined):SettingMove[]{
  if(!a || !b)return [];
  const out:SettingMove[]=[];
  if(a.plate!==b.plate){const setup=plateSetup(s,b.id),plate=(v:string|undefined)=>plateLabel(setup,v ?? setup?.default) || 'the default plate';
    out.push({label:'Build plate',from:plate(a.plate),to:plate(b.plate)});}
  const n=Math.max(1,b.slots.length),va=a.flushing?.volumes ?? [],vb=b.flushing?.volumes ?? [],one=Math.max(va.length,vb.length)>n*n;
  const nozzles=plateSetup(s,b.id)?.flushing?.nozzles ?? [];
  const volume=(v:(number|null)[],m:number[]|undefined,i:number,cell:number,nozzle:number)=>{
    const raw=v[i] ?? nozzles[nozzle]?.auto[cell];
    return raw===undefined || raw===null ? 'auto' : String(Math.round(Math.min(MAX_FLUSH_VOLUME,raw*(m?.[nozzle] ?? nozzles[nozzle]?.multiplier ?? 1))));
  };
  for(let i=0;i<Math.max(va.length,vb.length);i++)if((va[i] ?? null)!==(vb[i] ?? null)){
    const cell=i%(n*n),nozzle=Math.floor(i/(n*n)),from=Math.floor(cell/n)+1,to=cell%n+1;
    out.push({label:`Flushing volumes${one?` (nozzle ${nozzle+1})`:''}: slot ${from} → ${to},`,from:volume(va,a.flushing?.multipliers,i,cell,nozzle),to:volume(vb,b.flushing?.multipliers,i,cell,nozzle)});
  }
  const filamentName=(id:string|undefined)=>!id ? 'empty' : s.records.filaments.find((f)=>f.id===id || f.aliases?.includes(id))?.name ?? 'a removed filament';
  for(let i=0;i<Math.max(a.slots.length,b.slots.length);i++){
    const x=a.slots[i],y=b.slots[i];
    const removed=!!x?.filament && !library.filaments.some((f)=>(f.id===x.filament || f.aliases?.includes(x.filament)) && !f.archived);
    if((x?.filament ?? '')!==(y?.filament ?? '') && !removed)out.push({label:`Slot ${i+1}`,from:filamentName(x?.filament),to:filamentName(y?.filament)});
    if((x?.colour ?? '')!==(y?.colour ?? ''))out.push({label:`Slot ${i+1} colour`,from:x?.colour || 'none',to:y?.colour || 'none'});
  }
  const ma=a.flushing?.multipliers ?? [],mb=b.flushing?.multipliers ?? [];
  for(let i=0;i<Math.max(ma.length,mb.length);i++)if(ma[i]!==mb[i])out.push({label:`Flushing multiplier${Math.max(ma.length,mb.length)>1?` (nozzle ${i+1})`:''}`,from:String(ma[i] ?? 1),to:String(mb[i] ?? 1)});
  return out;
}
export const adoptingLibrary=()=>adopting;

function working(doc:LibraryRecords,pending:readonly LibraryWriteIntent[]):LibraryRecords{
  let out=doc;
  for(const intent of pending)if(intent.optimistic)for(const [key,next] of intent.change?.next ?? [])out=setRecord(out,key,next);
  return out;
}
function adoptLibrary(doc:LibraryRecords,published:LibraryWriteIntent[],written:Set<EntityKey>,pending:LibraryWriteIntent[]){
  if(doc.revision<getState().profileLibraryRevision && doc.epoch===getState().records.epoch){
    if(published.some(i=>i.publish))setState(s=>{let patch:Partial<State>={};for(const intent of published)if(intent.publish)patch={...patch,...intent.publish({...s,...patch},doc)};return patch;});
    return;
  }
  const before=getState(),library=working(doc,pending),remote:string[]=[];
  const own=new Set<EntityKey>([...written,...[...published,...pending].flatMap(i=>[...i.change?.next.keys() ?? []])]);
  for(const [type,list] of [['machine',library.printers],['filament',library.filaments],['process',library.processes]] as [PresetType,{id:string;revision:number}[]][])
    for(const r of list){const key=recordKey(kindOf(type),r.id),old=getRecord(before.records,key);if(old && !own.has(key) && !sameRecord(old,r as AnyRecord))remote.push(JSON.stringify([type,r.id]));}
  if(remote.length)void Promise.all([import('./format'),import('../options'),import('../state/value-equality')]).then(([{display},{OPTIONS},{sameValue}])=>{
    const conflicted:[string,import('../flow/profile-save').SettingMove[]][]=[],updated:[string,import('../flow/profile-save').SettingMove[]][]=[],rebased:Record<string,ProfileDraft>={};
    for(const k of remote){
      const [type,id]=JSON.parse(k) as [PresetType,string],rk=recordKey(kindOf(type),id),now=getRecord(library,rk);
      if((now as {archived?:unknown}|undefined)?.archived)continue;
      const diff=settingChanges(getRecord(before.records,rk),now),mine=draftChanges(before,type,id);
      const clash=diff.filter(([key,,b])=>key in mine && !sameValue(key,mine[key],b));
      if(clash.length){conflicted.push([k,clash.map(([key,,b])=>({label:OPTIONS[key]?.label||key,from:display(key,mine[key]),to:display(key,b)}))]);continue;}
      const draft=before.libraryDrafts[k];
      if(draft){const baseline={...draft.baseline};for(const [key,,b] of diff)if(b===undefined)delete baseline[key];else baseline[key]=b;
        rebased[k]=rebasedProfileDraft(draft,now as ProfileDraft['origin'],baseline);}
      const layers=resolvePreset(before,type,id).layers;
      const shown=(key:string,stored:ConfigValue|undefined)=>display(key,mine[key]??stored??valueBelow(layers[key],'record'));
      const told=[...diff.map(([key,a,b])=>({label:OPTIONS[key]?.label||key,from:shown(key,a),to:shown(key,b)})),
        ...(type==='machine'?printerMoves(before,library,getRecord(before.records,rk) as PrinterRecord|undefined,now as PrinterRecord|undefined):[])].filter(m=>m.from!==m.to);
      if(told.length)updated.push([k,told]);
    }
    if(Object.keys(rebased).length)setState(st=>({libraryDrafts:{...st.libraryDrafts,...Object.fromEntries(Object.entries(rebased).filter(([k])=>st.libraryDrafts[k]===before.libraryDrafts[k]))}}));
    if(conflicted.length)librarySync.send({type:'CONFLICT_ELSEWHERE',conflicts:conflicted});
    if(updated.length)librarySync.send({type:'RECORD_CHANGED_ELSEWHERE',moves:updated});});
  adopting=true;
  try{
    setState(s=>{
      let patch:Partial<State>={records:library,profileLibraryRevision:doc.revision,job:markStale(s.job),...removedInUseDrafts(s,library,(type,id)=>written.has(recordKey(kindOf(type),id)))};
      for(const [type,list] of [['machine',s.records.printers],['filament',s.records.filaments],['process',s.records.processes]] as [PresetType,{id:string;archived?:unknown}[]][])for(const r of list){
        const id=r.id,rk=recordKey(kindOf(type),id),k=JSON.stringify([type,id]);
        if(r.archived || written.has(rk) || !(getRecord(library,rk) as {archived?:unknown}|undefined)?.archived || (patch.libraryDrafts ?? s.libraryDrafts)[k] || profileSaveFor(k).get().value!=='naming')continue;
        const config=resolvePreset(s,type,id).config;
        patch={...patch,libraryDrafts:{...(patch.libraryDrafts ?? s.libraryDrafts),[k]:{values:config,baseline:config,origin:getRecord(s.records,rk) as ProfileDraft['origin'],printer:s.printer}}};}
      for(const intent of published)if(intent.publish)patch={...patch,...intent.publish({...s,...patch},doc)};
      return patch;
    });
  }finally{adopting=false;}
  const after=getState(),told:string[]=[];
  const name=(type:PresetType,id:string)=>(getRecord(before.records,recordKey(kindOf(type),id)) as {name?:string}|undefined)?.name ?? 'A profile';
  const gone=(type:PresetType,id:string)=>!!id && !written.has(recordKey(kindOf(type),id)) && !(getRecord(before.records,recordKey(kindOf(type),id)) as {archived?:unknown}|undefined)?.archived
    && (!getRecord(library,recordKey(kindOf(type),id)) || !!(getRecord(library,recordKey(kindOf(type),id)) as {archived?:unknown}).archived);
  const now=(type:PresetType,id:string)=>id ? (getRecord(after.records,recordKey(kindOf(type),id)) as {name?:string}|undefined)?.name : undefined;
  if(gone('machine',before.printer))told.push(`${name('machine',before.printer)} was removed in another window; choose a printer.`);
  if(gone('process',before.process))told.push(`${name('process',before.process)} was removed in another window; ${now('process',after.process) ? `${now('process',after.process)} is used now` : 'choose a print profile'}.`);
  const was=slotFilaments(before),is=slotFilaments(after);
  was.forEach((id,i)=>{if(gone('filament',id))told.push(`${name('filament',id)} was removed in another window; slot ${i+1} ${now('filament',is[i]) ? `now uses ${now('filament',is[i])}` : 'is empty'}.`);});
  if(told.length)librarySync.send({type:'IN_USE_REMOVED_ELSEWHERE',lines:told});
}

const ready=(s:State)=>{if(s.profileLibraryLoad.state==='failed')throw new Error(s.profileLibraryLoad.error);};
const put=(change:LibraryChange,doc:LibraryRecords,record:AnyRecord|undefined,key:EntityKey)=>{if(!change.base.has(key))change.base.set(key,getRecord(doc,key));change.next.set(key,record);};

export function commitRecords(records:{kind:RecordKind;record:AnyRecord;remove?:boolean; base?:AnyRecord}[],options:{label?:string;optimistic?:boolean;apply?(s:State):Partial<State>}={}):Promise<LibraryRecords>{
  const s=getState();
  try{ready(s);}catch(error){return Promise.reject(error);}
  const change:LibraryChange={base:new Map(),next:new Map()};
  for(const {kind,record,remove,base} of records){const key=recordKey(kind,record.id);if(base!==undefined)change.base.set(key,base);put(change,s.records,remove ? undefined : record,key);}
  const apply=options.apply;
  return submitLibraryWrite({label:options.label ?? 'Library',change,optimistic:options.optimistic,publish:apply?st=>apply(st):undefined});
}

type CreationState = 'idle' | 'committing' | 'done' | 'failed';
export interface CreationResult {keys:string[]}
type CreationEvent = { type: 'COMMIT' } | { type: 'DONE'; result: CreationResult } | { type: 'FAIL'; error: string };
interface CreationContext { result: CreationResult | null; error: string | null; flight: Promise<CreationResult> | null }
export type CreationMachine = Machine<CreationState, CreationEvent, CreationContext>;
export interface ProfileCreation {readonly machine:CreationMachine;commit():Promise<string>}
export const creationPending=(c:{machine:CreationMachine})=>c.machine.get().value==='committing';
function creationMachine(write:()=>Promise<CreationResult>):CreationMachine{
  const start={target:'committing' as const,action:()=>{
    const flight=write();
    flight.then(result=>machine.send({type:'DONE',result}),error=>machine.send({type:'FAIL',error:error instanceof Error?error.message:String(error)}));
    return {flight,error:null};
  }};
  const machine:CreationMachine=createMachine<CreationState,CreationEvent,CreationContext>({id:'profile-creation',initial:'idle',strict:true,context:{result:null,error:null,flight:null},states:{
    idle:{on:{COMMIT:start}},
    committing:{on:{COMMIT:{},DONE:{target:'done',action:(_,e)=>({result:e.result,flight:null})},FAIL:{target:'failed',action:(_,e)=>({error:e.error,flight:null})}}},
    done:{on:{COMMIT:{}}},
    failed:{on:{COMMIT:start}},
  }});
  return machine;
}
function commitCreation(machine:CreationMachine):Promise<CreationResult>{
  const {value,context}=machine.get();
  if(value==='done')return Promise.resolve(context.result!);
  machine.send({type:'COMMIT'});
  return machine.get().context.flight!;
}
export function recordCreation(make:(doc:LibraryRecords)=>{kind:RecordKind;record:AnyRecord}[],label='Add profile'):ProfileCreation{
  let keys:string[]=[];
  const write=async()=>{
    ready(getState());
    await submitLibraryWrite({label,make(doc){const planned=make(doc);keys=planned.map(p=>p.record.id);return {base:new Map(),next:new Map(planned.map(p=>[recordKey(p.kind,p.record.id),p.record]))};}});
    return {keys};
  };
  const machine=creationMachine(write);
  return {machine,commit:()=>commitCreation(machine).then(r=>r.keys[0])};
}

type NamedKind = 'printer' | 'filament' | 'process';
type Named = PrinterRecord | FilamentRecord | ProcessRecord;
const listOf = (doc: LibraryRecords, kind: NamedKind) => (kind === 'printer' ? doc.printers : kind === 'filament' ? doc.filaments : doc.processes) as Named[];
export function recordCopy<R extends AnyRecord>(kind:RecordKind,record:R,id:string,name:string):R{
  const {archived:_a,...rest}=record as R&{archived?:unknown};
  const copy={...rest,id,name,revision:0} as unknown as R;
  if(kind==='printer'){const p=copy as unknown as PrinterRecord;p.slots=p.slots.map(slot=>({...slot,id:`slot:${randomId()}`}));}
  if(kind==='filament')delete (copy as unknown as FilamentRecord).aliases;
  return copy;
}
export function copyRecord(kind:NamedKind,id:string,name:string):ProfileCreation{
  const copyId=`profile:${randomId()}`,origin=getRecord(getState().records,recordKey(kind,id));
  return recordCreation(doc=>{
    const current=getRecord(doc,recordKey(kind,id)) as Named|undefined;
    if(!current || !name.trim())throw new Error('Pick a profile to copy and give your new copy a name.');
    if(!sameRecord(current,origin))throw conflict('The profile you\'re copying was just updated elsewhere. Give it a quick look before duplicating.');
    if(listOf(doc,kind).some(r=>!r.archived && r.name===name && r.id!==copyId))throw new LibraryWriteError(`Oops! There's already a ${kind==='process'?'print profile':kind} named ${name}.`,'name_taken');
    const copy=recordCopy(kind,current,copyId,name);
    const out:{kind:RecordKind;record:AnyRecord}[]=[{kind,record:copy}];
    if(kind==='printer' || kind==='filament')for(const c of doc.calibrations.filter(c=>(kind==='printer'?c.printer:c.filament)===id))
      out.push({kind:'calibration',record:{...c,id:`calibration:${randomId()}`,...(kind==='printer'?{printer:copyId}:{filament:copyId}),revision:0}});
    return out;
  },'Duplicate');
}

export interface RecordRemoval {kind:NamedKind;ids:string[];operationId:string;slots:{printer:string;slot:string;filament:string}[]}
export async function removeRecords(kind:NamedKind,ids:readonly string[]):Promise<RecordRemoval>{
  const s=getState(),operationId=`archive:${randomId()}`,gone=new Set(ids),at=Date.now();
  const records:{kind:RecordKind;record:AnyRecord}[]=listOf(s.records,kind).filter(r=>gone.has(r.id) && !r.archived).map(r=>({kind,record:{...r,archived:{at,operationId}}}));
  if(records.length!==gone.size)throw new Error('That profile is out of sync—refresh your library before removing it.');
  const slots:RecordRemoval['slots']=[];
  if(kind==='filament')for(const p of s.records.printers)if(p.slots.some(slot=>gone.has(slot.filament))){
    for(const slot of p.slots)if(gone.has(slot.filament))slots.push({printer:p.id,slot:slot.id,filament:slot.filament});
    records.push({kind:'printer',record:{...p,slots:p.slots.map(slot=>gone.has(slot.filament)?{...slot,filament:''}:slot)}});
  }
  await commitRecords(records,{label:'Remove'});
  return {kind,ids:[...ids],operationId,slots};
}
export async function restoreRecords(removal:RecordRemoval):Promise<void>{
  const s=getState();
  const records:{kind:RecordKind;record:AnyRecord}[]=listOf(s.records,removal.kind).filter(r=>removal.ids.includes(r.id) && r.archived?.operationId===removal.operationId)
    .map(r=>{const {archived:_a,...rest}=r;return {kind:removal.kind,record:rest as AnyRecord};});
  if(!records.length)return;
  for(const p of s.records.printers){
    const back=removal.slots.filter(x=>x.printer===p.id);
    if(back.some(x=>p.slots.some(slot=>slot.id===x.slot && !slot.filament)))
      records.push({kind:'printer',record:{...p,slots:p.slots.map(slot=>!slot.filament ? {...slot,filament:back.find(x=>x.slot===slot.id)?.filament ?? ''} : slot)}});
  }
  await commitRecords(records,{label:'Restore'});
}

let following:Promise<void>=Promise.resolve();
export function followLibraryRevision(revision:number):Promise<void>{
  return following=following.then(async()=>{
    if(getState().profileLibraryLoad.state!=='ready' || revision<=getState().profileLibraryRevision || libraryWritesPending())return;
    const document=await readLibrary();
    if(libraryWritesPending() || document.revision<=getState().profileLibraryRevision)return;
    knowLibrary(document);adoptLibrary(document,[],new Set(),[]);
  }).catch(()=>{});
}
