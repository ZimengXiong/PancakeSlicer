import type { StoredDocBase } from '../persist/backend';
import type { ProjectDraftMeta, ProjectFailure } from '../persist/drafts';
export type { ProjectFailure } from '../persist/drafts';
export interface DraftCheckpoint { id: string; sequence: number }
export type CheckpointDocument = StoredDocBase & { recoveryCheckpoint?: DraftCheckpoint };
export type DraftDurability = { state: 'none' } | { state: 'pending'; checkpoint: DraftCheckpoint }
  | { state: 'durable'; checkpoint: DraftCheckpoint; updatedAt: number }
  | { state: 'failed'; checkpoint: DraftCheckpoint; message: string };
export const TRASHED = 'This project is hanging out in the Trash.';
export function projectFailure(error: unknown, projectId: string, expectedRevision: number, loading = false): ProjectFailure {
  const source = error && typeof error === 'object' ? error as { name?: unknown; message?: unknown; failure?: ProjectFailure } : {};
  if (source.failure?.projectId === projectId) return { ...source.failure };
  const name = typeof source.name === 'string' ? source.name : 'Error';
  const kind = name === 'ProjectConflictError' ? 'conflict' : name === 'ProjectDeletedError' ? 'deleted'
    : name === 'QuotaExceededError' ? 'quota' : name === 'ProjectServiceError' ? 'unavailable' : loading ? 'decode' : 'unavailable';
  const message = kind === 'deleted' && !loading ? TRASHED : kind === 'quota' ? 'Oops! Server storage is full. Keep this tab open and export your project as a file, or free up space and try saving again.'
    : typeof source.message === 'string' ? source.message : String(error);
  return { kind, name, message, projectId, expectedRevision };
}
export const conflictFailure = (projectId: string, expectedRevision: number): ProjectFailure => ({
  kind: 'conflict', name: 'ProjectConflictError', projectId, expectedRevision,
  message: 'Heads up: this project was edited in another window. Load the latest version or choose Keep mine to save your draft.',
});
export const DRAFT_CONFLICT_MESSAGE = 'Good news — we rescued some unsaved changes from before this project was last saved! Hit Reload latest for the saved version, or Keep mine to save these changes over it.';
export const draftConflictFailure = (projectId: string, expectedRevision: number): ProjectFailure => ({
  kind: 'conflict', name: 'ProjectDraftConflictError', projectId, expectedRevision, message: DRAFT_CONFLICT_MESSAGE,
});
export function failureError(failure: ProjectFailure): Error & { failure: ProjectFailure } {
  return Object.assign(new Error(failure.message), { name: failure.name, failure });
}
export const blocksAutosave = (failure: ProjectFailure | null) => failure?.kind === 'conflict' || failure?.kind === 'deleted' || failure?.kind === 'interrupted';
export const RECOVERED_MESSAGE = 'We recovered your unsaved changes! Click Retry to save them, or reload the last saved version.';
export function checkpointCommitted(draft: Pick<ProjectDraftMeta, 'id' | 'sequence'>, doc: CheckpointDocument): boolean {
  return doc.recoveryCheckpoint?.id === draft.id && doc.recoveryCheckpoint.sequence >= draft.sequence;
}
export function recoveryDocument(doc: CheckpointDocument): StoredDocBase {
  const { recoveryCheckpoint: _checkpoint, ...document } = doc;
  return document;
}
export interface DraftCodec<D> {
  encode(document: D): { stored: StoredDocBase; blobs: Map<string, Blob> };
  nameOf(document: D): string; plateCount(document: D): number;
}
