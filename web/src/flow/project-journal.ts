import type { DraftRef, ProjectDraft, ProjectDraftStore, ProjectFailure } from '../persist/drafts';
import type { DraftCheckpoint, DraftCodec } from './project-recovery';
import type { ProjectDraftInventory } from './project';
export function createDraftJournal<D>(owner: string, storage: () => Promise<ProjectDraftStore>, codec: () => Promise<DraftCodec<D>>,
  beginMutation: (id: string) => (error?: unknown) => void = () => () => {}) {
  let serial = 0, tail = Promise.resolve();
  const failures = new Map<string, unknown>();
  type Capture = { checkpoint: DraftCheckpoint; baseRevision: number; document: D; failure: ProjectFailure | null; editedAt: number; supersedes: readonly DraftRef[] };
  const pending = new Map<string, { latest: Capture; durable: Promise<ProjectDraft>; release(): void }>();
  return {
    capture(projectId: string, baseRevision: number, document: D, failure: ProjectFailure | null = null, editedAt = Date.now(), supersedes: readonly DraftRef[] = []) {
      const checkpoint = { id: `${owner}.${projectId}`, sequence: ++serial };
      const finish = beginMutation(checkpoint.id);
      const queued = pending.get(projectId);
      const latest = { checkpoint, baseRevision, document, failure, editedAt, supersedes: [...(queued?.latest.supersedes ?? []), ...supersedes] };
      if (queued) {
        queued.latest = latest; void queued.durable.then(() => finish(), (error: unknown) => finish(error));
        return { checkpoint, durable: queued.durable };
      }
      let release!: () => void;
      const delay = new Promise<void>((resolve) => {
        const timer = setTimeout(resolve, 250);
        release = () => { clearTimeout(timer); resolve(); };
      });
      const batch = { latest, durable: null! as Promise<ProjectDraft>, release };
      const durable = tail.then(async () => {
        await delay;
        pending.delete(projectId);
        const { checkpoint, baseRevision, document, failure, editedAt, supersedes } = batch.latest;
        const { encode, nameOf, plateCount } = await codec(), { stored, blobs } = encode(document);
        const draft: ProjectDraft = { ...checkpoint, owner, projectId, baseRevision, doc: stored,
          name: nameOf(document), objects: plateCount(document), updatedAt: Date.now(), editedAt, kind: 'dirty', failure,
          blobs: Object.fromEntries(blobs) };
        await (await storage()).put(draft, supersedes); failures.delete(projectId); return draft;
      });
      batch.durable = durable; pending.set(projectId, batch);
      void durable.then(() => finish(), (error: unknown) => finish(error));
      tail = durable.then(() => {}, (error) => { failures.set(projectId, error); });
      return { checkpoint, durable };
    },
    async flush(projectId?: string) {
      for (const batch of pending.values()) batch.release();
      for (;;) { const waiting = tail; await waiting; if (waiting === tail) break; }
      const error = projectId === undefined ? failures.values().next().value : failures.get(projectId);
      if (error !== undefined) throw error;
    },
  };
}

export async function readDraftInventory(storage: () => Promise<ProjectDraftStore>, previous: ProjectDraftInventory): Promise<ProjectDraftInventory> {
  try { return { status: 'ready', drafts: await (await storage()).listMetadata(), error: null }; }
  catch (error) { return { ...previous, status: 'error', error: error instanceof Error ? error.message : String(error) }; }
}
