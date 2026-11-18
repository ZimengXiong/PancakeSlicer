import { openingBusy } from '../state/busy';
import { useSyncExternalStore } from 'react';
import { clearPlate } from '../actions';
import { clearPlateJobs } from '../plater/plates';
import { newKey, projectStore, projectStorageError, type ProjectMeta, type ProjectStore } from '../persist/backend';
import type { ProjectDraftMeta } from '../persist/drafts';
import type { DraftCheckpoint, DraftDurability, ProjectFailure } from './project-recovery';
import { documentGeneration, leaveDocument, newDocument, onLeaveDocument, sameContents, snapshot, subscribeHistory, type Doc } from '../state/history';
import { getState, setState, subscribeStore, toast } from '../state/store';
import { tools } from './tools';
import { assertNever, createMachine, useMachine, type Machine, type Snapshot } from './fsm';
import { currentRoute, writeRoute } from './routes';
import type { createDraftJournal } from './project-journal';
import { importing } from './import';
import { writesDiscarded } from '../api/access';
import { versionShown, versionView, versionWanted } from './version-view';
import { acquireLease, followProjectLease, leaseRefused, leaseSettled, leaseWritable } from './project-lease';

export type { ProjectMeta } from '../persist/backend';
export { newProject } from '../actions';
export type ProjectStateName = 'blank' | 'opening' | 'editing' | 'saving' | 'saved' | 'failed' | 'conflict' | 'deleted' | 'missing';
export interface ProjectContext {
  id: string;
  name: string;
  savedAt: number;
  revision: number;
  error: string | null;
  dirty: boolean;
  loadError: boolean;
  origin: 'new' | 'loaded' | 'saved';
  failure: ProjectFailure | null;
  draft: DraftDurability;
  loaded: { revision: number; objects: number } | null;
  allowEmpty: boolean;
}
export type ProjectEvent =
  | { type: 'EDIT' } | { type: 'FLUSH' } | { type: 'RETRY' } | { type: 'BLANK' } | { type: 'OPEN' }
  | { type: 'SAVED'; id: string; name: string; savedAt: number; revision: number; objects?: number; checkpoint?: DraftCheckpoint }
  | { type: 'ERROR'; error: string; failure?: ProjectFailure }
  | { type: 'DRAFT'; id: string; durability: Exclude<DraftDurability, { state: 'none' }> }
  | { type: 'RECOVERED'; meta: ProjectMeta; draft: ProjectDraftMeta; failure: ProjectFailure; baseObjects: number }
  | { type: 'OPENED'; meta: ProjectMeta } | { type: 'MISSING'; id: string } | { type: 'DELETED' }
  | { type: 'REBASE'; revision: number } | { type: 'REMOTE' } | { type: 'EMPTIED' } | { type: 'RESTORED'; revision: number } | { type: 'SAVE_AS_NEW' };
let recoveryCore: typeof import('./project-recovery');
const projectFailure = (...args: Parameters<typeof recoveryCore.projectFailure>) => recoveryCore.projectFailure(...args);
const conflictFailure = (...args: Parameters<typeof recoveryCore.conflictFailure>) => recoveryCore.conflictFailure(...args);
const failureError = (...args: Parameters<typeof recoveryCore.failureError>) => recoveryCore.failureError(...args);
const blocksAutosave = (failure: ProjectFailure | null) => failure ? recoveryCore?.blocksAutosave(failure) ?? true : false;
const AUTOSAVE_MS = 1000;
const codec = () => import('../persist/session');
let store: ProjectStore | null = null;
let observed: Doc | null = null;
let queue: Promise<void> = Promise.resolve();
const known = new Set<string>();
const removed = new Set<string>();
let generation = documentGeneration();
let epoch = 0;
let opening = 0;
let adopting = false;
let routing = false;
let previous: string | null = null;
let leaving = false;
const sameDoc = (a: Doc | null, b: Doc) => !!a && (Object.keys(b) as (keyof Doc)[]).every((k) => k === 'selected' || a[k] === b[k]);
const errorText = (e: unknown) => e instanceof Error ? e.message : String(e);
const blank = (): ProjectContext => ({ id: newKey(), name: 'Untitled', savedAt: 0, revision: 0, error: null, dirty: false, loadError: false, origin: 'new', failure: null, draft: { state: 'none' },
  loaded: { revision: 0, objects: 0 }, allowEmpty: false });
const objectCount = (d: Doc) => d.objects.length + d.projectPlates.filter((p) => p.index !== d.plateIndex).reduce((n, p) => n + p.objects.length, 0);
const activeWrite = (id: string, gen: number, version: number) =>
  id === project.get().context.id && gen === documentGeneration() && version === epoch;

const draftStorage = async () => (await import('../persist/drafts')).projectDraftStore();
const session = { owner: newKey() };
let leaseFor: string | null = null;
async function leaseForSave(): Promise<boolean> {
  const { id, revision } = project.get().context;
  if (revision <= 0 || !id) return true;
  leaseFor = id;
  return acquireLease(id);
}
const journalTools = () => import('./project-journal');
let draftActivity: typeof import('../persist/drafts') | null = null;
let journal: ReturnType<typeof createDraftJournal<Doc>>;
let editedAt = 0, restoredEdit: number | null = null;
let captured: { id: string; document: Doc; revision: number; failure: ProjectFailure | null; receipt: ReturnType<typeof journal.capture> } | null = null;
let restoredDraft: ProjectDraftMeta | null = null;
export type ProjectDraftInventory = { status: 'loading' | 'ready' | 'error'; drafts: readonly ProjectDraftMeta[]; error: string | null };
let draftInventory: ProjectDraftInventory = { status: 'loading', drafts: [], error: null }, draftListToken = 0;
const draftListeners = new Set<() => void>();
export const projectDraftSnapshot = () => draftInventory;
export const subscribeProjectDrafts = (listener: () => void) => { draftListeners.add(listener); return () => { draftListeners.delete(listener); }; };
async function readProjectDrafts(): Promise<ProjectDraftInventory> {
  const mine = ++draftListToken;
  const result = await (await journalTools()).readDraftInventory(draftStorage, draftInventory);
  if (mine === draftListToken) draftInventory = result;
  if (mine === draftListToken) draftListeners.forEach((listener) => listener());
  return draftInventory;
}
let draftRead: Promise<ProjectDraftInventory> | null = null, draftReadNext: Promise<ProjectDraftInventory> | null = null;
export function refreshProjectDrafts(): Promise<ProjectDraftInventory> {
  if (!draftRead) {
    const read = readProjectDrafts().finally(() => { if (draftRead === read) draftRead = null; });
    return draftRead = read;
  }
  return draftReadNext ??= draftRead.catch(() => {}).then(() => { draftReadNext = null; return refreshProjectDrafts(); });
}
function checkpointProject(document = snapshot(), id = project.get().context.id, revision = project.get().context.revision,
  failure = project.get().context.failure) {
  if (project.get().context.draft.state !== 'failed' && captured?.id === id && captured.revision === revision && captured.failure === failure && sameDoc(captured.document, document)) return captured.receipt;
  if (restoredEdit !== null) { editedAt = restoredEdit; restoredEdit = null; }
  else if (!captured || !sameDoc(captured.document, document)) editedAt = Date.now();
  const adopted = restoredDraft?.projectId === id && restoredDraft.id !== `${session.owner}.${id}` ? restoredDraft : null;
  const gen = documentGeneration(), version = epoch,
    receipt = journal.capture(id, revision, document, failure, editedAt, adopted ? [{ id: adopted.id, sequence: adopted.sequence }] : []);
  if (adopted) void receipt.durable.then(() => { if (restoredDraft === adopted) restoredDraft = null; }, () => {});
  captured = { id, document, revision, failure, receipt };
  project.send({ type: 'DRAFT', id, durability: { state: 'pending', checkpoint: receipt.checkpoint } });
  void receipt.durable.then((draft) => {
    if (activeWrite(id, gen, version) && project.get().value !== 'saved') project.send({ type: 'DRAFT', id, durability: { state: 'durable', checkpoint: receipt.checkpoint, updatedAt: draft.updatedAt } });
    void refreshProjectDrafts();
  }, (error: unknown) => {
    if (activeWrite(id, gen, version)) project.send({ type: 'DRAFT', id, durability: { state: 'failed', checkpoint: receipt.checkpoint, message: errorText(error) } });
  });
  return receipt;
}
const UNSAVED = 'unsavedProject';
export function writeProjectRoute(replace = false) {
  const { id, savedAt } = project.get().context;
  if (savedAt) { writeRoute(`/p/${id}`, replace); return; }
  writeRoute('/', replace);
  if (hasUnsavedProject()) history.replaceState({ ...history.state, [UNSAVED]: id }, '');
}
let retryDelay = 1000, retries = 0, retryTimer: ReturnType<typeof setTimeout> | undefined;
function retryUnavailable() {
  const { value, context } = project.get();
  if (value === 'saved') { retryDelay = 1000; retries = 0; }
  if (value !== 'failed' || context.failure?.kind !== 'unavailable' || retryTimer || retries >= 30) return;
  const again = () => {
    clearTimeout(retryTimer); retryTimer = undefined; window.removeEventListener('online', again);
    const now = project.get();
    if (now.value === 'failed' && now.context.failure?.kind === 'unavailable') { retries++; void retryProject(); }
  };
  retryTimer = setTimeout(again, retryDelay); window.addEventListener('online', again);
  retryDelay = Math.min(retryDelay * 2, 10_000);
}
function routeSavedProject() {
  const c = project.get().context;
  if (c.savedAt && !routing && getState().tab !== 'projects' && currentRoute().kind === 'blank') writeRoute(`/p/${c.id}`, true);
}
function hasUnsavedProject() {
  const { value, context } = project.get();
  return value === 'editing' || value === 'saving' || value === 'conflict' || value === 'failed' && !context.loadError;
}
function writeRefusal(c: ProjectContext, doc: Doc): ProjectFailure | null {
  if (!c.loaded || c.loaded.revision !== c.revision) return { kind: 'interrupted', name: 'ProjectNotLoadedError', projectId: c.id, expectedRevision: c.revision,
    message: "Oops! This project didn't finish loading, so we paused auto-save to protect your file. Click Retry to load it again." };
  if (objectCount(doc) > 0 || c.loaded.objects === 0 || c.allowEmpty) return null;
  const saved = c.loaded.objects > 0 ? `the saved project has ${c.loaded.objects} object${c.loaded.objects === 1 ? '' : 's'}` : "the saved project couldn't be read";
  return { kind: 'conflict', name: 'ProjectEmptyError', projectId: c.id, expectedRevision: c.revision,
    message: `This tab's plate is empty (${saved}). Click Reload latest to keep the saved models, or Keep mine to save the empty plate.` };
}
function save(id: string, m: Machine<ProjectStateName, ProjectEvent, ProjectContext>) {
  const doc = snapshot(), gen = documentGeneration(), version = epoch, revision = m.get().context.revision;
  const refusal = writeRefusal(m.get().context, doc);
  if (refusal) { queue = queue.then(() => { if (activeWrite(id, gen, version)) m.send({ type: 'ERROR', error: refusal.message, failure: refusal }); }); return; }
  const storage = store, receipt = checkpointProject(doc, id, revision);
  queue = queue.then(async () => {
    await (await recoveryActions()).saveProjectSnapshot(recoveryRuntime, { id, document: doc, gen, version, revision, storage, receipt });
  }).catch((error: unknown) => {
    if (leaseRefused(error, id)) { if (activeWrite(id, gen, version)) m.send({ type: 'EDIT' }); return; }
    const failure = projectFailure(error, id, revision);
    if (activeWrite(id, gen, version)) {
      m.send({ type: 'ERROR', error: failure.message, failure });
      checkpointProject(snapshot(), id, revision, failure);
    }
  });
}

const failureOf = (c: ProjectContext, e: { error: string; failure?: ProjectFailure }) => e.failure ?? projectFailure(e.error, c.id, c.revision);
const failedWith = (c: ProjectContext, e: { error: string; failure?: ProjectFailure }) => { const failure = failureOf(c, e); return { error: failure.message, failure, dirty: true }; };
const failed = [
  { target: 'conflict', guard: (c: ProjectContext, e: { error: string; failure?: ProjectFailure }) => failureOf(c, e).kind === 'conflict', action: failedWith },
  { target: 'failed', action: failedWith },
] as const;
const draftUpdate = { guard: (c: ProjectContext, e: Extract<ProjectEvent, { type: 'DRAFT' }>) => c.id === e.id &&
  (c.draft.state === 'none' || c.draft.checkpoint.sequence <= e.durability.checkpoint.sequence),
  action: (_: ProjectContext, e: Extract<ProjectEvent, { type: 'DRAFT' }>) => ({ draft: e.durability }) };
const recoveredWith = (_: ProjectContext, e: Extract<ProjectEvent, { type: 'RECOVERED' }>): Partial<ProjectContext> => ({
  ...e.meta, revision: e.draft.baseRevision, origin: 'loaded', error: e.failure.message, failure: e.failure, dirty: true, loadError: false,
  loaded: { revision: e.draft.baseRevision, objects: e.baseObjects }, allowEmpty: false,
  draft: { state: 'durable', checkpoint: { id: e.draft.id, sequence: e.draft.sequence }, updatedAt: e.draft.updatedAt },
});
const recovered = [
  { target: 'conflict', guard: (_: ProjectContext, e: Extract<ProjectEvent, { type: 'RECOVERED' }>) => e.failure.kind === 'conflict', action: recoveredWith },
  { target: 'failed', action: recoveredWith },
] as const;
const leave = { ERROR: failed, DRAFT: draftUpdate, RECOVERED: recovered, BLANK: 'blank', OPEN: { target: 'opening', action: () => ({ loaded: null }) }, DELETED: 'deleted',
  MISSING: { target: 'missing', action: (_: ProjectContext, e: { id: string }) => ({ id: e.id, dirty: false, loaded: null }) },
  EMPTIED: { action: () => ({ allowEmpty: true }) } } as const;
const writable = (c: ProjectContext) => !blocksAutosave(c.failure) && leaseWritable();
const notices = (unsaved: boolean) => ({ REMOTE: { target: 'conflict', action: (c: ProjectContext) => ({ failure: conflictFailure(c.id, c.revision), dirty: unsaved || c.dirty }) } }) as const;
const savedPatch = (c: ProjectContext, e: Extract<ProjectEvent, { type: 'SAVED' }>): Partial<ProjectContext> => ({
  name: e.name, savedAt: e.savedAt, revision: e.revision, origin: 'saved', dirty: false, error: null, failure: null,
  loaded: { revision: e.revision, objects: e.objects ?? -1 }, allowEmpty: false,
  ...(e.checkpoint && c.draft.state !== 'none' && c.draft.checkpoint.sequence <= e.checkpoint.sequence ? { draft: { state: 'none' } } : {}),
});

export const project = createMachine<ProjectStateName, ProjectEvent, ProjectContext>({
  id: 'project', initial: 'blank', context: blank(),
  states: {
    blank: { entry: blank,
      on: { ...leave, EDIT: 'saving' } },
    opening: { on: { ...leave,
      OPENED: { target: 'saved', action: (_, e) => ({ ...e.meta, revision: e.meta.revision ?? 0, origin: 'loaded', error: null, failure: null, draft: { state: 'none' }, dirty: false, loadError: false,
        loaded: { revision: e.meta.revision ?? 0, objects: e.meta.objects ?? -1 }, allowEmpty: false }) },
      ERROR: { target: 'failed', action: (c, e) => ({ error: e.error, failure: e.failure ?? projectFailure(e.error, c.id, c.revision, true), loadError: true }) },
    } },
    editing: {
      after: { ms: AUTOSAVE_MS, target: 'saving', guard: writable },
      on: { ...leave, ...notices(true), EDIT: 'editing', FLUSH: { target: 'saving', guard: writable } } },
    saving: { entry: (ctx, _e, m) => { save(ctx.id, m); return { dirty: false }; },
      on: { ...leave, ...notices(false), EDIT: { action: () => ({ dirty: true }) },
        SAVED: [
          { target: 'editing', guard: (ctx, e) => ctx.id === e.id && ctx.dirty, action: savedPatch },
          { target: 'saved', guard: (ctx, e) => ctx.id === e.id, action: savedPatch },
        ],
        ERROR: failed,
      } },
    saved: { on: { ...leave, ...notices(false), EDIT: 'editing' } },
    failed: {
      on: { ...leave, ...notices(true), EDIT: { action: () => ({ dirty: true }) },
        RETRY: { target: 'saving', guard: (c) => c.failure?.kind !== 'deleted', action: () => ({ failure: null, error: null }) },
        RESTORED: { target: 'saving', guard: (c) => c.failure?.kind === 'deleted', action: (c, e) => ({ revision: e.revision, loaded: c.loaded && { ...c.loaded, revision: e.revision }, failure: null, error: null }) },
        SAVE_AS_NEW: { target: 'saving', guard: (c) => c.failure?.kind === 'deleted',
          action: () => ({ id: newKey(), revision: 0, savedAt: 0, origin: 'new' as const, failure: null, error: null, loaded: { revision: 0, objects: 0 }, allowEmpty: true }) } } },
    conflict: {
      on: { ...leave, REMOTE: {}, EDIT: { action: () => ({ dirty: true }) },
        ERROR: { action: (c, e) => { const failure = c.failure ?? failureOf(c, e); return { error: failure.message, failure, dirty: true }; } },
        SAVED: { guard: (c, e) => c.id === e.id, action: (c, e) => {
          const failure = c.failure ?? conflictFailure(c.id, e.revision);
          return { ...savedPatch(c, e), dirty: true, failure, error: failure.message };
        } },
        REBASE: { target: 'editing', action: (c, e) => ({ revision: e.revision, error: null, failure: null, loaded: c.loaded && { ...c.loaded, revision: e.revision }, allowEmpty: true }) } } },
    deleted: { entry: blank, on: { ...leave, EDIT: 'editing' } },
    missing: { on: leave },
  },
});

let list: ProjectMeta[] | null = null;
export type ProjectListSnapshot = {state:'loading'|'ready'|'failed';error:string|null};
let listStatus:ProjectListSnapshot={state:'loading',error:null};
export const projectListSnapshot=()=>listStatus;
export const subscribeProjectList=(listener:()=>void)=>{listeners.add(listener);return()=>{listeners.delete(listener);};};
export async function refreshProjects(){try{if(!store)store=await projectStore(true);refreshList();}catch(error){listStatus={state:'failed',error:errorText(error)};listeners.forEach(f=>f());}}
const listeners = new Set<() => void>();
let listing = 0;
function refreshList() {
  const mine = ++listing;
  listStatus={state:'loading',error:null};listeners.forEach(f=>f());
  if (!store) { listStatus={state:'failed',error:'Oops! Can\'t reach project storage right now. Try reconnecting to the service.'};listeners.forEach(f=>f());return; }
  void store.list().then((l) => {
    if (mine !== listing) return;
    list = l;listStatus={state:'ready',error:null}; listeners.forEach((f) => f());
  }).catch((e: unknown) => {if(mine!==listing)return;listStatus={state:'failed',error:errorText(e)};listeners.forEach(f=>f());});
}
function adopt(f: () => void) {
  adopting = true;
  try { f(); } finally { adopting = false; leaving = false; }
  generation = documentGeneration();
  observed = snapshot();
}
export const openingStep = (f: () => void) => project.get().value === 'saved' ? adopt(f) : f();

const sender = newKey();
let channel: BroadcastChannel | null = null;
function publish(type: 'saved' | 'deleted', id: string) {
  const message = { type, id, sender, at: Date.now() };
  channel?.postMessage(message);
}
function remote(message: { type?: string; id?: string; sender?: string }) {
  if (!message || typeof message !== 'object' || message.sender === sender || !message.id) return;
  refreshList();
  if (message.id !== project.get().context.id) return;
  if (message.type === 'deleted') {
    removed.add(message.id); epoch++; opening++;
    if (hasUnsavedProject()) {
      const failure = projectFailure({ name: 'ProjectDeletedError', message: recoveryCore.TRASHED }, message.id, project.get().context.revision);
      project.send({ type: 'ERROR', error: failure.message, failure }); checkpointProject();
    } else { project.send({ type: 'MISSING', id: message.id }); notFound(); writeRoute('/', true); void route(); }
  } else if (message.type === 'saved') {
    project.send({ type: 'REMOTE' }); checkpointProject();
  }
}

let routeVersion = 0;
const notFound = () => toast('project_not_found', undefined, { action: { label: 'Projects', run: () => showProjects() } });
async function route() {
  const mine = ++routeVersion;
  const r = currentRoute();
  routing = true;
  try {
    if (r.kind === 'projects') {
      opening++; await flushProject().catch(() => {});
      if (mine !== routeVersion) return;
      if ((project.get().value === 'opening' || project.get().value === 'missing')) project.send({ type: 'BLANK' });
      openingBusy.end(); setState({ tab: 'projects' });
    }
    else if (r.kind === 'project') {
      await openProject(r.id, { history: false });
      if (mine === routeVersion && project.get().value === 'missing') { notFound(); writeRoute('/', true); await route(); }
    }
    else if (r.kind === 'blank') {
      versionView.send({ type: 'LATEST' });
      await flushProject();
      if (mine !== routeVersion) return;
      epoch++; opening++;
      adopt(() => { leaveDocument(); clearPlateJobs(); clearPlate(); newDocument(true); });
      project.send({ type: 'BLANK' }); setState({ tab: 'prepare' });
    } else { writeRoute('/', true); await route(); }
  } catch (e) { toast('project_failed', { detail: errorText(e) }); }
  finally { if (mine === routeVersion) routing = false; }
}
let started: Promise<void> | null = null;
export function startProjects(ready: Promise<unknown> = Promise.resolve()): Promise<void> {
  if (started) return started;
  started = start(ready);
  window.orcaDesktop?.onBeforeClose?.(async () => { await started; await flushProject(); await (await import('../saved-preferences')).flushSavedPreferences(); });
  window.orcaDesktop?.onPendingSaves?.(async () => (hasUnsavedProject() ? 1 : 0)
    + ((await import('../saved-preferences')).savedPreferencesSnapshot().state === 'pending' ? 1 : 0));
  return started;
}
async function start(ready: Promise<unknown>) {
  const modules = Promise.all([import('./project-recovery'), import('../persist/drafts'), journalTools()]);
  const listed = projectStore();
  await ready.catch(() => {});
  const [recovery, drafts, { createDraftJournal }] = await modules;
  recoveryCore = recovery; draftActivity = drafts;
  store = await listed ?? await projectStore(true);
  journal = createDraftJournal<Doc>(session.owner, draftStorage, codec, (id) => draftActivity!.beginProjectDraftMutation(id));
  void (async () => {
    const [{ watchBlankSetup, forgetBlankSetup }, { loadServiceSession }] = await Promise.all([import('../persist/blank-setup'), import('../persist/service-session')]);
    await loadServiceSession();
    await new Promise<void>((resolve) => { if (getState().profileLibraryLoad.state === 'ready') { resolve(); return; } const stop = subscribeStore(() => { if (getState().profileLibraryLoad.state === 'ready') { stop(); resolve(); } }); });
    watchBlankSetup(() => project.get().value === 'blank');
    let wasBlank = project.get().value === 'blank';
    project.subscribe(({ value }) => { if (wasBlank && value === 'saving') forgetBlankSetup(); wasBlank = value === 'blank'; });
  })().catch(() => {});
  observed = snapshot();
  const observe = () => {
    if (tools.get().value === 'dragging') return;
    if (adopting || versionShown() || leaving) return;
    if (project.get().value === 'opening' || project.get().value === 'missing' || project.get().context.loadError || !project.get().context.loaded) {
      if (getState().tab === 'projects' && !routing) showProjects();
      return;
    }
    const doc = snapshot();
    const before = observed; observed = doc;
    const empty = !doc.project && !doc.projectName && !doc.objects.length && !doc.projectPlates.some((p) => p.objects.length);
    const uneditedBlank = empty && (project.get().value === 'blank' || project.get().value === 'deleted');
    if (!sameDoc(before, doc) && !(before && sameContents(before, doc)) && !uneditedBlank) {
      checkpointProject(doc);
      project.send({ type: 'EDIT' });
      if (getState().tab !== 'projects' && !routing) writeProjectRoute(!project.get().context.savedAt);
    }
    if (!routing) {
      const r = currentRoute(), tab = getState().tab;
      if (tab === 'projects' && r.kind !== 'projects') writeRoute('/projects');
      else if (tab !== 'projects' && r.kind === 'projects') writeProjectRoute();
    }
  };
  subscribeStore(observe);
  tools.subscribe(observe);
  project.subscribe(routeSavedProject);
  project.subscribe(retryUnavailable);
  onLeaveDocument(() => {
    if (versionShown()) return;
    if (hasUnsavedProject()) checkpointProject();
    project.send({ type: 'FLUSH' });
    leaving = true;
    queueMicrotask(() => { if (importing.machine.get().value !== 'running') leaving = false; });
  });
  importing.machine.subscribe((s) => { if (s.value !== 'running') leaving = false; });
  subscribeHistory(() => {
    const c = project.get().context;
    if (!adopting && !versionShown() && c.loaded && c.loaded.objects !== 0 && !c.allowEmpty && documentGeneration() === generation && objectCount(snapshot()) === 0) project.send({ type: 'EMPTIED' });
  });
  subscribeHistory(() => {
    leaving = false;
    if (adopting || documentGeneration() === generation) return;
    versionView.send({ type: 'LATEST' });
    if (hasUnsavedProject() && observed) checkpointProject(observed);
    generation = documentGeneration(); epoch++; opening++;
    previous = project.get().context.id;
    observed = snapshot();
    project.send({ type: 'BLANK' });
    if (!routing) writeRoute('/');
    if (observed.objects.length || observed.project) {
      checkpointProject(observed);
      project.send({ type: 'EDIT' });
      if (!routing) writeProjectRoute(true);
    }
  });
  const flush = () => { void flushProject().catch(() => {}); };
  window.addEventListener('pagehide', flush);
  window.addEventListener('beforeunload', (e) => {
    if (window.orcaDesktop || writesDiscarded()) return;
    const v = project.get().value;
    if (project.get().context.loadError || (v !== 'editing' && v !== 'saving' && v !== 'failed' && !(v === 'conflict' && project.get().context.dirty))) return;
    flush(); e.preventDefault(); e.returnValue = '';
  });
  document.addEventListener('visibilitychange', () => { if (document.visibilityState === 'hidden') flush(); });
  window.addEventListener('popstate', () => void route());
  window.addEventListener('hashchange', () => void route());
  if (typeof BroadcastChannel !== 'undefined') {
    channel = new BroadcastChannel('orca.projects'); channel.onmessage = (e) => remote(e.data);
  }
  const leased = () => {
    const { value, context } = project.get();
    if (context.revision <= 0 || !context.id) return null;
    if (value === 'editing' || value === 'saving') leaseFor = context.id;
    return leaseFor === context.id ? context.id : null;
  };
  followProjectLease(leased, (f) => project.subscribe(f),
    (from, id) => from === 'viewing' && !hasUnsavedProject() ? void openProject(id, { reload: true }) : project.send({ type: 'FLUSH' }));
  refreshList(); void refreshProjectDrafts();
  const unsaved: unknown = currentRoute().kind === 'blank' ? history.state?.[UNSAVED] : null;
  if (currentRoute().kind === 'blank' && observed && (observed.objects.length || observed.project)) {
    checkpointProject(observed);
    project.send({ type: 'EDIT' }); writeProjectRoute(true);
  } else if (!(typeof unsaved === 'string' && await resumeUnsaved(unsaved)) && !(currentRoute().kind === 'blank' && await resumeOrphaned())) await route();
  if (!store) project.send({ type: 'ERROR', error: projectStorageError() });
  (await import('./boot')).startFlows();
}

export const storing = () => !!store;
export const previousProject = () => previous;
export const openWithUnsaved = (id: string) => project.get().context.id === id && hasUnsavedProject();
export async function flushProjectDraft(): Promise<void> {
  const id = project.get().context.id;
  if (hasUnsavedProject()) await checkpointProject().durable;
  await journal.flush(id);
}
export async function flushProject(): Promise<void> {
  const initial = project.get();
  if (initial.value === 'conflict' || initial.value === 'failed') {
    if (hasUnsavedProject()) await checkpointProject().durable.catch(() => {});
    throw failureError(initial.context.failure ?? projectFailure(initial.context.error ?? 'Project not saved', initial.context.id, initial.context.revision));
  }
  const id = project.get().context.id;
  do {
    await leaseSettled();
    if (!leaseWritable()) throw failureError(projectFailure('This project is currently being edited in another tab or device. Click Take control to edit and save from here.', id, project.get().context.revision));
    project.send({ type: 'FLUSH' });
    await queue;
    if (project.get().context.id !== id) return;
    if (project.get().value === 'failed' || project.get().value === 'conflict') throw failureError(project.get().context.failure ?? projectFailure(project.get().context.error ?? 'Project not saved', id, project.get().context.revision));
  } while (project.get().value === 'editing' || project.get().value === 'saving');
}
export async function retryProject() {
  try {
    if (!store) store = await projectStore(true);
    if (!store) throw new Error(projectStorageError());
    const r = currentRoute();
    if ((project.get().context.loadError || project.get().context.failure?.name === 'ProjectNotLoadedError') && r.kind === 'project') {
      await openProject(r.id, { reload: true }); void (await import('./boot')).startFlows(); return;
    }
    await (await recoveryActions()).dropDuplicateDrafts(recoveryRuntime);
    if (!await leaseForSave()) return;
    known.clear(); project.send({ type: 'RETRY' }); await flushProject();
    void (await import('./boot')).startFlows();
  } catch (e) {
    const c = project.get().context, failure = projectFailure(e, c.id, c.revision);
    project.send({ type: 'ERROR', error: failure.message, failure });
  }
}
export function showProjects() { writeRoute('/projects'); void route(); }
export const openProject = async (id: string, options: { history?: boolean; reload?: boolean } = {}) => {
  if (versionShown()) options = { ...options, reload: true };
  versionView.send({ type: 'LATEST' });
  return (await recoveryActions()).openProject(recoveryRuntime, id, options);
};
export const viewProjectVersion = async (id: string, record: { meta: ProjectMeta; doc: import('../persist/backend').StoredDocBase }, request: number) =>
  (await recoveryActions()).viewProjectVersion(recoveryRuntime, id, record, () => {
    if (!versionWanted(request)) return false;
    versionView.send({ type: 'SHOWN', request });
    return true;
  });
async function resumeUnsaved(id: string) {
  routing = true;
  try {
    const resumed = await (await recoveryActions()).resumeUnsavedProject(recoveryRuntime, id);
    if (!resumed) history.replaceState({ ...history.state, [UNSAVED]: undefined }, '');
    return resumed;
  }
  catch (e) { toast('project_failed_3', { detail: errorText(e) }); return false; }
  finally { routing = false; routeSavedProject(); }
}
async function resumeOrphaned() {
  routing = true;
  try { return await (await recoveryActions()).resumeOrphanedProject(recoveryRuntime); }
  catch (e) { toast('resume_orphaned_failed', { detail: errorText(e) }); return false; }
  finally { routing = false; routeSavedProject(); }
}
export const leaveProjectVersion = (id: string) => openProject(id, { reload: true });
export async function restoreProjectVersion() {
  if (!versionShown()) return;
  versionView.send({ type: 'RESTORE' });
  observed = snapshot();
  checkpointProject(observed);
  project.send({ type: 'EDIT' });
  await flushProject();
}

const recoveryActions = () => import('./project-recovery-actions');
const recoveryRuntime: import('./project-recovery-actions').RecoveryRuntime = {
  storage: () => store, removed, checkpoint: () => checkpointProject(),
  hasUnsaved: hasUnsavedProject, waitForWrites: () => queue, beginOpen: () => ++opening,
  invalidateWrites: () => { epoch++; }, isCurrent: (mine) => mine === opening, adopt,
  previous: (id) => { previous = id; }, restored: () => restoredDraft, setRestored: (draft) => { restoredDraft = draft; },
  resetCaptured: () => { captured = null; }, restoreEditedAt: (at: number) => { restoredEdit = at; }, checkpointRecovered: (id, revision, failure) => checkpointProject(snapshot(), id, revision, failure),
  drafts: draftStorage, refreshDrafts: refreshProjectDrafts, known, session, publish, refreshList, activeWrite,
};
export const keepProjectVersion = async () => { if (await leaseForSave()) await (await recoveryActions()).keepProjectVersion(recoveryRuntime); };
export const reloadProject = async () => (await recoveryActions()).reloadProject(recoveryRuntime);

export const renameProject = async (id: string, name: string) => (await recoveryActions()).renameProject(recoveryRuntime, id, name);
export const duplicateProject = async (id: string): Promise<ProjectMeta | null> => (await recoveryActions()).duplicateProject(recoveryRuntime, id);
export const deleteProject = async (id: string): Promise<() => Promise<void>> => (await recoveryActions()).deleteProject(recoveryRuntime, id);
export const restoreArchivedProject = async (id: string, expectedRevision?: number) => (await recoveryActions()).restoreArchivedProject(recoveryRuntime, id, expectedRevision);
export async function restoreAndSave() {
  const c = project.get().context;
  try { const ack = await restoreArchivedProject(c.id); removed.delete(c.id); project.send({ type: 'RESTORED', revision: ack.revision }); await flushProject(); }
  catch (e) { const failure = projectFailure(e, c.id, c.revision); project.send({ type: 'ERROR', error: failure.message, failure }); }
}
export async function saveAsNewProject() { project.send({ type: 'SAVE_AS_NEW' }); await flushProject(); writeProjectRoute(true); }
export const discardRejectedProjectCommand = async (operationId: string) => (await recoveryActions()).discardRejectedProjectCommand(recoveryRuntime, operationId);

export type ProjectSnapshot = Snapshot<ProjectStateName, ProjectContext>;
export function useProject(): ProjectSnapshot;
export function useProject<T>(sel: (s: ProjectSnapshot) => T): T;
export function useProject<T>(sel?: (s: ProjectSnapshot) => T) { return useMachine(project, sel); }
export const projectList = () => list;
export const useProjects = () => useSyncExternalStore((l) => (listeners.add(l), () => void listeners.delete(l)), () => list);
(window as unknown as { __project: unknown }).__project = { machine: project, list: () => list };

export type ProjectLifecycle = { value: 'new' | 'loading' | 'ready' | 'dirty' | 'saving' | 'saved'; id: string }
  | { value: 'conflict'; id: string; dirty: boolean; failure: ProjectFailure }
  | { value: 'missing'; id: string } | { value: 'failed'; id: string; error: string; failure: ProjectFailure };
export function projectLifecycle(): ProjectLifecycle {
  const { value, context: c } = project.get();
  switch (value) {
    case 'conflict': return { value, id: c.id, dirty: c.dirty, failure: c.failure ?? conflictFailure(c.id, c.revision) };
    case 'blank': case 'deleted': return { value: 'new', id: c.id };
    case 'opening': return { value: 'loading', id: c.id };
    case 'editing': return { value: 'dirty', id: c.id };
    case 'saving': return { value, id: c.id };
    case 'saved': return { value: c.origin === 'loaded' ? 'ready' : 'saved', id: c.id };
    case 'missing': return { value, id: c.id };
    case 'failed': return { value, id: c.id, error: c.error ?? 'Project unavailable', failure: c.failure ?? projectFailure(c.error ?? 'Project unavailable', c.id, c.revision) };
    default: return assertNever(value);
  }
}
