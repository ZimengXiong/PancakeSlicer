import { documentText } from '../persist/wire';
import { openingBusy } from '../state/busy';
import { newKey, type ProjectMeta, type ProjectStore, type ProjectCommandStatus, type StoredDocBase } from '../persist/backend';
import { changeForSave, savedVersionBase } from '../projects/versions';
import type { ProjectDraft, ProjectDraftMeta, ProjectDraftStore } from '../persist/drafts';
import type { StoredDoc } from '../persist/session';
import { clearPlate } from '../actions';
import { documentGeneration, leaveDocument, newDocument, snapshot, type Doc } from '../state/history';
import { clearPlateJobs } from '../plater/plates';
import { IDLE_JOB, setState } from '../state/store';
import { project, projectList, flushProject, writeProjectRoute, type ProjectDraftInventory } from './project';
import { writeRoute } from './routes';
import { checkpointCommitted, conflictFailure, draftConflictFailure, failureError, projectFailure, TRASHED, recoveryDocument, type DraftCheckpoint, type ProjectFailure, type CheckpointDocument, RECOVERED_MESSAGE } from './project-recovery';
export interface RecoveryRuntime {
  storage(): ProjectStore | null; removed: Set<string>;
  checkpoint(): { checkpoint: DraftCheckpoint; durable: Promise<ProjectDraft> };
  hasUnsaved(): boolean; waitForWrites(): Promise<void>;
  beginOpen(): number; invalidateWrites(): void; isCurrent(mine: number): boolean; adopt(action: () => void): void;
  previous(id: string): void; restored(): ProjectDraftMeta | null; setRestored(draft: ProjectDraftMeta | null): void; resetCaptured(): void;
  restoreEditedAt(at: number): void;
  checkpointRecovered(id: string, revision: number, failure: ProjectFailure): unknown;
  activeWrite(id: string, gen: number, epoch: number): boolean;
  known: Set<string>; session: { owner: string };
  publish(type: 'saved' | 'deleted', id: string): void; refreshList(): void;
  drafts(): Promise<ProjectDraftStore>; refreshDrafts(): Promise<ProjectDraftInventory>;
}
const codec = () => import('../persist/session');
const editedOf = (d: ProjectDraftMeta) => d.editedAt ?? d.updatedAt;
async function documentOffer(document: StoredDoc): Promise<() => void> {
  const m = await import('./project-slots');
  const offer = await m.documentOffer(document as Parameters<typeof m.documentOffer>[0]);
  return () => m.settleOffer(offer);
}
export async function adoptProjectDraft(runtime: RecoveryRuntime, draft: ProjectDraft, canonical: ProjectMeta | null, mine: number, { offer = false } = {}) {
  const document = recoveryDocument(draft.doc) as StoredDoc;
  if (!runtime.isCurrent(mine)) return;
  const offered = documentOffer(document);
  const storage = await runtime.drafts();
  const fields = await (await codec()).decode(document, async (key) => draft.blobs[key] ?? await storage.asset(key));
  const settleOffer = await offered;
  if (!runtime.isCurrent(mine)) return;
  const meta: ProjectMeta = canonical ?? { id: draft.projectId, name: draft.name, revision: draft.baseRevision,
    savedAt: 0, objects: draft.objects, thumbnail: null };
  const failure = runtime.removed.has(draft.projectId) ? projectFailure({ name: 'ProjectDeletedError', message: TRASHED }, draft.projectId, draft.baseRevision)
    : canonical && (canonical.revision ?? 0) !== draft.baseRevision ? draftConflictFailure(draft.projectId, draft.baseRevision)
    : draft.failure?.kind === 'conflict' && (canonical || draft.baseRevision > 0) ? draftConflictFailure(draft.projectId, draft.baseRevision)
    : { kind: 'interrupted', name: 'ProjectInterruptedError', projectId: draft.projectId, expectedRevision: draft.baseRevision,
      message: RECOVERED_MESSAGE } satisfies ProjectFailure;
  runtime.previous(project.get().context.id);
  runtime.adopt(() => {
    leaveDocument(); clearPlateJobs();
    setState({ ...fields, projectName: draft.name, tab: 'prepare', gizmo: 'none', dialog: null,
      job: IDLE_JOB });
    newDocument(false);
    settleOffer();
  });
  runtime.setRestored(draft); runtime.resetCaptured(); runtime.restoreEditedAt(editedOf(draft));
  project.send({ type: 'RECOVERED', meta, draft, failure, baseObjects: canonical ? canonical.objects ?? -1 : draft.baseRevision > 0 ? -1 : 0 });
  if (!offer) runtime.checkpointRecovered(draft.projectId, draft.baseRevision, failure);
  writeProjectRoute(true);
}
export async function resumeUnsavedProject(runtime: RecoveryRuntime, id: string): Promise<boolean> {
  const mine = runtime.beginOpen();
  const [rec, drafts] = await Promise.all([runtime.storage()?.load(id) ?? null,
    runtime.drafts().then((storage) => storage.listMetadata(id)).catch(() => [] as ProjectDraftMeta[])]);
  if (!runtime.isCurrent(mine)) return true;
  if (rec) { await openProject(runtime, id, { history: false }); return true; }
  const newest = drafts.filter((d) => d.kind === 'dirty' && d.baseRevision === 0)
    .sort((a, b) => editedOf(b) - editedOf(a))[0];
  const draft = newest && await (await runtime.drafts()).load(newest.id);
  if (!draft || !runtime.isCurrent(mine)) return !!draft;
  project.send({ type: 'OPEN' });
  await adoptProjectDraft(runtime, draft, null, mine);
  if (runtime.isCurrent(mine) && project.get().context.id === id) project.send({ type: 'RETRY' });
  return true;
}
export async function resumeOrphanedProject(runtime: RecoveryRuntime): Promise<boolean> {
  const mine = `${runtime.session.owner}.`;
  const orphans = (await (await runtime.drafts()).listMetadata())
    .filter((d) => d.kind === 'dirty' && d.baseRevision === 0 && !d.id.startsWith(mine)).sort((a, b) => editedOf(b) - editedOf(a));
  for (const draft of orphans) {
    if (await runtime.storage()?.load(draft.projectId)) continue;
    return resumeUnsavedProject(runtime, draft.projectId);
  }
  return false;
}
export async function dropDuplicateDrafts(runtime: RecoveryRuntime) {
  const source = runtime.restored();
  if (!source || source.kind !== 'dirty') return;
  const storage = await runtime.drafts(), offered = await storage.load(source.id);
  if (!offered) return;
  const body = JSON.stringify(offered.doc);
  const siblings = (await storage.listMetadata(source.projectId))
    .filter((d) => d.id !== source.id && d.kind === 'dirty' && d.baseRevision === source.baseRevision && editedOf(d) <= editedOf(source));
  for (const meta of siblings) {
    const draft = await storage.load(meta.id);
    if (draft && JSON.stringify(draft.doc) === body) await storage.remove(draft.id, draft.sequence);
  }
  if (siblings.length) void runtime.refreshDrafts();
}
export async function keepProjectVersion(runtime: RecoveryRuntime) {
  const id = project.get().context.id, gen = documentGeneration();
  try {
    await dropDuplicateDrafts(runtime);
    await runtime.checkpoint().durable;
    await runtime.waitForWrites();
    const rec = await runtime.storage()?.load(id);
    if (id !== project.get().context.id || gen !== documentGeneration()) return;
    if (!rec || runtime.removed.has(id)) throw failureError(projectFailure({ name: 'ProjectDeletedError', message: 'Oops! We couldn\'t find the saved project on the server, but you can save your local draft as a new copy.' }, id, project.get().context.revision));
    project.send({ type: 'REBASE', revision: rec.meta.revision ?? 0 });
    runtime.resetCaptured();
    await flushProject();
  } catch (error) {
    const failure = projectFailure(error, id, project.get().context.revision);
    if (id === project.get().context.id && gen === documentGeneration()) project.send({ type: 'ERROR', error: failure.message, failure });
  }
}
export async function reloadProject(runtime: RecoveryRuntime) {
  await dropDuplicateDrafts(runtime);
  const id = project.get().context.id, gen = documentGeneration();
  const receipt = runtime.hasUnsaved() ? runtime.checkpoint() : null, source = runtime.restored();
  await receipt?.durable.catch(() => null);
  if (id !== project.get().context.id || gen !== documentGeneration()) return;
  await openProject(runtime, id, { reload: true });
  const opened = project.get();
  if (opened.context.id === id && (opened.value === 'editing' || opened.value === 'saved') && !opened.context.dirty && receipt) {
    await (await runtime.drafts()).remove(receipt.checkpoint.id, receipt.checkpoint.sequence);
    if (source?.kind === 'dirty') await (await runtime.drafts()).remove(source.id, source.sequence);
    void runtime.refreshDrafts();
  }
}
export async function openProject(runtime: RecoveryRuntime, id: string, options: { history?: boolean; reload?: boolean } = {}) {
  if (!options.reload && project.get().context.id === id && project.get().value !== 'missing' && project.get().value !== 'opening' && !project.get().context.loadError) {
    setState({ tab: 'prepare' }); if (options.history !== false) writeRoute(`/p/${id}`); return;
  }
  const mine = runtime.beginOpen();
  if (!options.reload && !project.get().context.loadError) await flushProject();
  else { runtime.invalidateWrites(); await runtime.waitForWrites(); }
  if (!runtime.isCurrent(mine)) return;
  runtime.invalidateWrites();
  project.send({ type: 'OPEN' }); setState({ tab: 'prepare' });
  const busy = openingBusy.begin('Opening');
  if (options.history !== false) writeRoute(`/p/${encodeURIComponent(id)}`);
  try {
    let loadError: unknown;
    let rec: Awaited<ReturnType<ProjectStore['load']>> = null;
    try { const store = runtime.storage(); if (!store) throw new Error('Oops! Project storage isn\'t available right now.'); rec = await store.load(id); } catch (error) { loadError = error; }
    const drafts: ProjectDraftMeta[] = await runtime.drafts().then((storage) => storage.listMetadata(rec?.meta.id ?? id)).catch((error: unknown) => {
      throw new Error(`Couldn't check this project for unsaved changes, so it wasn't opened: ${error instanceof Error ? error.message : String(error)}`, { cause: error });
    });
    if (!runtime.isCurrent(mine)) return;
    const draft = !options.reload ? drafts.filter((d) => d.kind === 'dirty')
      .sort((a, b) => editedOf(b) - editedOf(a))[0] : undefined;
    if (draft && (!rec || !checkpointCommitted(draft, rec.doc))) {
      const restored = await (await runtime.drafts()).load(draft.id);
      if (!runtime.isCurrent(mine)) return;
      if (restored && rec && documentText(recoveryDocument(restored.doc)) === documentText(recoveryDocument(rec.doc))) await (await runtime.drafts()).remove(draft.id, draft.sequence).catch(() => {});
      else if (restored) { await adoptProjectDraft(runtime, restored, rec?.meta ?? null, mine, { offer: true }); return; }
    }
    if (loadError) throw loadError;
    if (!rec || runtime.removed.has(rec.meta.id)) { project.send({ type: 'MISSING', id }); return; }
    const document = recoveryDocument(rec.doc) as StoredDoc;
    if (!runtime.isCurrent(mine)) return;
    const offered = documentOffer(document);
    const fields = await (await codec()).decode(document, (k) => runtime.storage()!.getBlob(rec.meta.id, k));
    const settleOffer = await offered;
    if (!runtime.isCurrent(mine)) return;
    if (runtime.removed.has(rec.meta.id)) { project.send({ type: 'MISSING', id }); return; }
    for (const k of Object.keys(rec.doc.files ?? {})) runtime.known.add(`${rec.meta.id}/${k}`);
    runtime.previous(project.get().context.id);
      runtime.adopt(() => {
      leaveDocument(); clearPlateJobs();
      setState({ ...fields, projectName: rec.meta.name, tab: 'prepare', gizmo: 'none', dialog: null,
        job: IDLE_JOB });
      newDocument(true);
      settleOffer();
    });
    runtime.setRestored(null); runtime.resetCaptured(); savedVersionBase(rec.meta.id, snapshot());
    project.send({ type: 'OPENED', meta: rec.meta });
      writeRoute(`/p/${rec.meta.id}`, true);
  } catch (error) {
    if (!runtime.isCurrent(mine)) return;
    const failure = projectFailure(error, id, project.get().context.revision, true);
    project.send({ type: 'ERROR', error: failure.message, failure });
  } finally {
    busy.end();
  }
}
export async function viewProjectVersion(runtime: RecoveryRuntime, id: string, record: { meta: ProjectMeta; doc: StoredDocBase }, enter: () => boolean) {
  const mine = runtime.beginOpen();
  await flushProject();
  if (!runtime.isCurrent(mine) || project.get().context.id !== id) return false;
  const document = recoveryDocument(record.doc) as StoredDoc;
  if (!runtime.isCurrent(mine)) return false;
  const offered = documentOffer(document);
  const fields = await (await codec()).decode(document, (k) => runtime.storage()!.getBlob(id, k));
  const settleOffer = await offered;
  if (!runtime.isCurrent(mine)) return false;
  if (!enter()) return false;
  runtime.invalidateWrites();
  runtime.adopt(() => {
    leaveDocument(); clearPlateJobs();
    setState({ ...fields, projectName: record.meta.name, gizmo: 'none', dialog: null, job: IDLE_JOB });
    newDocument(true);
    settleOffer();
  });
  return true;
}
export async function renameProject(runtime: RecoveryRuntime, id: string, name: string) {
  if (!name.trim()) return;
  const storage = runtime.storage();
  if (!storage) throw new Error('Oops! Project storage isn\'t available right now.');
  const current = id === project.get().context.id, gen = documentGeneration();
  let resumed = false;
  for (const pending of storage.commands()) if (pending.id === id && pending.kind === 'rename' && pending.name !== undefined) {
    await storage.rename(id, pending.name); resumed ||= pending.name === name.trim();
  }
  if (current) {
    if (id === project.get().context.id && gen === documentGeneration()) setState({ projectName: name.trim() });
    return;
  }
  if (resumed) { runtime.publish('saved', id); runtime.refreshList(); return; }
  await runtime.waitForWrites();
  await storage.rename(id, name.trim()); runtime.publish('saved', id); runtime.refreshList();
}
interface PendingAction<T> { run(): Promise<T>; matches(command: Readonly<ProjectCommandStatus>): boolean; task: Promise<T> | null }
const duplicateIntents = new WeakMap<RecoveryRuntime, Map<string, PendingAction<ProjectMeta>>>();
const deleteIntents = new WeakMap<RecoveryRuntime, Map<string, PendingAction<() => Promise<void>>>>();
const removalOwners = new WeakMap<RecoveryRuntime, Map<string, { revision: number | null; dismissedRestore: boolean }>>();
function actionMap<T>(owners: WeakMap<RecoveryRuntime, Map<string, PendingAction<T>>>, runtime: RecoveryRuntime) {
  let map = owners.get(runtime);
  if (!map) { map = new Map(); owners.set(runtime, map); }
  return map;
}
function attemptAction<T>(map: Map<string, PendingAction<T>>, id: string, create: () => Omit<PendingAction<T>, 'task'>): Promise<T> {
  let intent = map.get(id);
  if (!intent) { intent = { ...create(), task: null }; map.set(id, intent); }
  if (!intent.task) {
    const retained = intent, task = Promise.resolve().then(retained.run);
    retained.task = task;
    void task.then(() => { if (map.get(id) === retained) map.delete(id); }, () => {}).finally(() => {
      if (retained.task === task) retained.task = null;
    });
  }
  return intent.task!;
}
export function discardRejectedProjectCommand(runtime: RecoveryRuntime, operationId: string): void {
  const storage = runtime.storage();
  if (!storage) throw new Error('Oops! Project storage isn\'t available right now.');
  const command = storage.commands().find((entry) => entry.operationId === operationId);
  if (!command || command.state !== 'conflict') throw new Error('Wait until the save conflict finishes resolving before dismissing this notice.');
  const map = command.kind === 'duplicate' ? duplicateIntents.get(runtime) : command.kind === 'archive' ? deleteIntents.get(runtime) : undefined;
  const action = map?.get(command.id), matching = action?.matches(command);
  if (matching && action?.task) throw new Error('Hang tight—waiting for the last action to finish before dismissing.');
  storage.discardRejectedCommand(operationId);
  if (matching && map && map.get(command.id) === action) map.delete(command.id);
  const owner = removalOwners.get(runtime)?.get(command.id);
  if (command.kind === 'restore' && owner?.revision === command.expectedRevision) owner.dismissedRestore = true;
}
export async function duplicateProject(runtime: RecoveryRuntime, id: string): Promise<ProjectMeta | null> {
  const store = runtime.storage();
  if (!store) return null;
  return attemptAction(actionMap(duplicateIntents, runtime), id, () => {
    const newId = newKey(), name = `${projectList()?.find((p) => p.id === id)?.name ?? project.get().context.name} copy`;
    const current = id === project.get().context.id, gen = documentGeneration();
    let prepared = false, meta: ProjectMeta | null = null;
    return { matches: (command) => command.kind === 'duplicate' && command.newId === newId && command.name === name, run: async () => {
      if (!prepared) {
        if (current && id === project.get().context.id && gen === documentGeneration()) await flushProject();
        await runtime.waitForWrites(); prepared = true;
      }
      meta ??= await store.duplicate(id, newId, name);
      runtime.publish('saved', meta.id); runtime.refreshList(); return meta;
    } };
  });
}
export async function deleteProject(runtime: RecoveryRuntime, id: string): Promise<() => Promise<void>> {
  const storage = runtime.storage();
  if (!storage) throw new Error('Oops! Project storage isn\'t available right now.');
  return attemptAction(actionMap(deleteIntents, runtime), id, () => {
    const gen = documentGeneration(), current = id === project.get().context.id, owner = { revision: null as number | null, dismissedRestore: false };
    let owners = removalOwners.get(runtime);
    if (!owners) { owners = new Map(); removalOwners.set(runtime, owners); }
    const retainedOwners = owners;
    let ack: Awaited<ReturnType<ProjectStore['remove']>> | null = null, restored = false, restoring: Promise<void> | null = null;
    const undo = () => {
      if (owner.dismissedRestore) return Promise.reject(new Error('That undo couldn\'t be applied and was dismissed. Check your project before restoring an older version.'));
      if (restored) return Promise.resolve();
      if (!restoring) {
        const task = Promise.resolve().then(async () => {
          if (retainedOwners.get(id) !== owner) throw new Error('This project was deleted more recently—use the latest Undo button to bring it back.');
          await storage.restore(id, ack!.revision);
          restored = true;
          if (retainedOwners.get(id) === owner) { retainedOwners.delete(id); runtime.removed.delete(id); }
          runtime.publish('saved', id); runtime.refreshList();
        });
        restoring = task;
        void task.finally(() => { if (restoring === task) restoring = null; }).catch(() => {});
      }
      return restoring;
    };
    return { matches: (command) => command.kind === 'archive', run: async () => {
      if (!ack) { await runtime.waitForWrites(); ack = await storage.remove(id); }
      owner.revision = ack.revision; retainedOwners.set(id, owner); runtime.removed.add(id);
      if (current && id === project.get().context.id && gen === documentGeneration()) {
        runtime.invalidateWrites(); runtime.beginOpen(); project.send({ type: 'DELETED' });
        runtime.adopt(() => { clearPlateJobs(); clearPlate(); newDocument(true); });
        setState({ tab: 'prepare' }); writeRoute('/');
      }
      runtime.publish('deleted', id); runtime.refreshList(); return undo;
    } };
  });
}

export async function restoreArchivedProject(runtime: RecoveryRuntime, id: string, expectedRevision?: number) {
  const storage = runtime.storage();
  if (!storage) throw new Error('Oops! Project storage isn\'t available right now.');
  const owner = removalOwners.get(runtime)?.get(id), removed = runtime.removed.has(id);
  const ack = await storage.restore(id, expectedRevision);
  if (removed && removalOwners.get(runtime)?.get(id) === owner) { removalOwners.get(runtime)?.delete(id); runtime.removed.delete(id); }
  runtime.publish('saved', id); runtime.refreshList();
  return ack;
}

const trashed = (id: string, revision: number) => failureError(projectFailure({ name: 'ProjectDeletedError', message: TRASHED }, id, revision));

export async function saveProjectSnapshot(runtime: RecoveryRuntime, capture: {
  id: string; document: Doc; gen: number; version: number; revision: number; storage: ProjectStore | null;
  receipt: { checkpoint: DraftCheckpoint; durable: Promise<ProjectDraft> };
}) {
  const { id, document, gen, version, revision, storage, receipt } = capture;
    if (revision > 0) await receipt.durable.catch(() => null);
    if (!storage) throw new Error('Oops! Project storage isn\'t available right now.');
    if (runtime.removed.has(id)) { if (runtime.activeWrite(id, gen, version)) throw trashed(id, revision); return; }
    if (runtime.activeWrite(id, gen, version) && project.get().value === 'conflict') throw failureError(project.get().context.failure ?? conflictFailure(id, revision));
    const { encode, nameOf, plateCount, thumbnail } = await codec();
    const { stored, blobs } = encode(document);
    for (const [k, b] of blobs) {
      if (runtime.known.has(`${id}/${k}`)) continue;
      await storage.putBlob(id, k, b);
      runtime.known.add(`${id}/${k}`);
    }
    if (runtime.removed.has(id)) { if (runtime.activeWrite(id, gen, version)) throw trashed(id, revision); return; }
    const meta = { name: nameOf(document), savedAt: Date.now(), objects: plateCount(document), revision, change: changeForSave(id, document, revision) };
    const committed: CheckpointDocument = { ...stored, recoveryCheckpoint: receipt.checkpoint };
    const acknowledgement = await storage.save(id, meta, committed, meta.objects === 0 ? null : runtime.activeWrite(id, gen, version) ? thumbnail() ?? undefined : undefined);
    savedVersionBase(id, document);
    if (runtime.activeWrite(id, gen, version)) project.send({ type: 'SAVED', id, ...meta, ...acknowledgement, checkpoint: receipt.checkpoint });
    if (!runtime.activeWrite(id, gen, version) || project.get().value !== 'conflict') {
      await receipt.durable.catch(() => null);
      const drafts = await runtime.drafts().catch(() => null);
      await drafts?.remove(receipt.checkpoint.id, receipt.checkpoint.sequence).catch(() => {});
      if (runtime.activeWrite(id, gen, version) && runtime.restored()?.projectId === id && runtime.restored()?.kind === 'dirty') {
        await drafts?.remove(runtime.restored()!.id, runtime.restored()!.sequence).catch(() => {}); runtime.setRestored(null);
      }
      void runtime.refreshDrafts();
    }
    runtime.publish('saved', id); runtime.refreshList();
}
