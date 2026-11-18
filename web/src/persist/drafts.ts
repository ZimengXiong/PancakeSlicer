import type { StoredDocBase } from './backend';
import { putAsset, readAsset, requireWorkspaceService, routeKey, serviceRequest } from './service';
import { captureProjectWire, prepareOnce, projectJson as serviceJson, projectWrite } from './project-wire';
import { projectId } from './identity';
export interface ProjectFailure {
  kind: 'conflict' | 'deleted' | 'quota' | 'unavailable' | 'interrupted' | 'decode';
  name: string; message: string; projectId: string; expectedRevision: number;
}
export interface ProjectDraftMeta {
  id: string; owner: string; projectId: string; sequence: number; baseRevision: number;
  name: string; objects: number; updatedAt: number; kind: 'dirty'; failure: ProjectFailure | null;
  editedAt?: number;
}
export interface ProjectDraft extends ProjectDraftMeta { doc: StoredDocBase; blobs: Record<string, Blob> }
export interface ProjectDraftStore {
  list(projectId?: string): Promise<ProjectDraft[]>;
  listMetadata(projectId?: string): Promise<ProjectDraftMeta[]>;
  load(id: string): Promise<ProjectDraft | null>;
  asset(key: string): Promise<Blob | null>;
  put(draft: ProjectDraft, supersedes?: readonly DraftRef[]): Promise<void>;
  remove(id: string, throughSequence: number): Promise<void>;
}
export interface DraftRef { id: string; sequence: number }
export interface ProjectDraftMutationSnapshot {
  readonly pending: number; readonly error: string | null;
}
let pending = 0, mutationSnapshot: ProjectDraftMutationSnapshot = { pending: 0, error: null };
const failures = new Map<string, string>();
const publishMutations = () => { mutationSnapshot = { pending, error: failures.values().next().value ?? null }; };
export const projectDraftMutationSnapshot = (): ProjectDraftMutationSnapshot => mutationSnapshot;
export function beginProjectDraftMutation(id: string): (error?: unknown) => void {
  pending++; publishMutations();
  let ended = false;
  return (error) => {
    if (ended) return; ended = true;
    pending--;
    if (error === undefined) failures.delete(id); else failures.set(id, error instanceof Error ? error.message : String(error));
    publishMutations();
  };
}
async function mutate<T>(id: string, operation: () => Promise<T>): Promise<T> {
  const finish = beginProjectDraftMutation(id);
  try { await requireWorkspaceService(); const result = await operation(); finish(); return result; }
  catch (error) { finish(error); throw error; }
}
interface ServiceDraft { id: string; sequence: number; document: Omit<ProjectDraft, 'blobs'> | null; metadata?: ProjectDraftMeta; updatedAt: number }
function validMetadata(meta: ProjectDraftMeta | undefined): meta is ProjectDraftMeta {
  return !!meta && typeof meta.id === 'string' && typeof meta.projectId === 'string' && typeof meta.owner === 'string' &&
    typeof meta.name === 'string' && Number.isSafeInteger(meta.sequence) && meta.sequence >= 0 &&
    Number.isSafeInteger(meta.baseRevision) && meta.baseRevision >= 0 && Number.isFinite(meta.updatedAt) && (meta.editedAt === undefined || Number.isFinite(meta.editedAt)) &&
    Number.isSafeInteger(meta.objects) && meta.objects >= 0 && meta.kind === 'dirty' &&
    (meta.failure === null || !!meta.failure && ['conflict', 'deleted', 'quota', 'unavailable', 'interrupted', 'decode'].includes(meta.failure.kind) &&
      typeof meta.failure.name === 'string' && typeof meta.failure.message === 'string' && meta.failure.projectId === meta.projectId && Number.isSafeInteger(meta.failure.expectedRevision));
}
const draftPath = (key: string) => `/api/drafts/${routeKey(key)}`;
const acknowledged = new Map<string, number>();
const pendingDrafts = new Map<string, Promise<RequestInit>>();
const serviceDraftStore: ProjectDraftStore = {
  async list(projectId) {
    const metadata = await this.listMetadata(projectId);
    return Promise.all(metadata.map(async (meta) => {
      const draft = await this.load(meta.id);
      if (!draft) throw new Error('Heads up: a recovery draft updated while loading. Refresh the page and try again.');
      return draft;
    }));
  },
  async listMetadata(projectId) {
    const records = await serviceJson<ServiceDraft[]>('/api/drafts');
    if (!Array.isArray(records)) throw new Error('Oops! Couldn\'t read the list of saved drafts from the server.');
    return records.map((record) => {
      if (!validMetadata(record.metadata) || record.metadata.id !== record.id || record.metadata.sequence !== record.sequence) throw new Error('Oops! A saved draft is missing some details.');
      return record.metadata;
    }).filter((meta) => projectId === undefined || meta.projectId === projectId).sort((a,b) => b.updatedAt - a.updatedAt);
  },
  async load(id) {
    const record = await serviceJson<ServiceDraft>(draftPath(id), undefined, true);
    if (!record) return null;
    const document = record.document;
    if (!document || !validMetadata(document) || !document.doc || document.id !== id || document.sequence !== record.sequence) throw new Error('Oops! The recovery draft data looks corrupted.');
    return { ...document, blobs: {} };
  },
  async asset(key) {
    const blob = await readAsset(key);
    if (!blob) throw new Error(`Oops! A 3D model file needed for recovery is missing: ${key}`);
    return blob;
  },
  put: (draft, supersedes = []) => mutate(draft.id, async () => {
    const { blobs: _blobs, ...document } = draft;
    const payload = { sequence: draft.sequence, document, metadata: draftMetadata(draft), supersedes };
    const capture = await captureProjectWire(payload);
    const key = `${draft.id}/${draft.sequence}/${capture.digest}`;
    const preparation = prepareOnce(pendingDrafts, key, async () => projectWrite(await capture.draft(projectId())));
    let request: RequestInit;
    try { request = await preparation; } finally { capture.release(); }
    for (const key of Object.keys(draft.doc.files ?? {})) {
      const blob = draft.blobs[key];
      if (!(blob instanceof Blob)) throw new Error(`Oops! A 3D model file in this draft is missing: ${key}`);
      await putAsset(key, blob);
    }
    const ack = await serviceJson<{sequence:number}>(draftPath(draft.id), request, false, 'project');
    if (!ack || ack.sequence !== draft.sequence) throw new Error('Oops! The server returned an unexpected response while saving your draft.');
    acknowledged.set(draft.id, Math.max(acknowledged.get(draft.id) ?? 0, ack.sequence));
    if (pendingDrafts.get(key) === preparation) pendingDrafts.delete(key);
  }),
  remove: (id, throughSequence) => mutate(id, async () => {
    if ((acknowledged.get(id) ?? 0) > throughSequence) return;
    try { await (await serviceRequest(`${draftPath(id)}?throughSequence=${throughSequence}`, { method: 'DELETE' }, true))?.text(); }
    catch (error) {
      if ((error as {status?:number}).status === 409) return;
      throw error;
    }
  }),
};
export async function projectDraftStore(): Promise<ProjectDraftStore> {
  await requireWorkspaceService();
  return serviceDraftStore;
}
export const draftMetadata = ({ doc: _doc, blobs: _blobs, ...meta }: ProjectDraft): ProjectDraftMeta => meta;
