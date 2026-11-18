import { newKey, type ProjectMeta, type ProjectStore, type StoredDocBase, type ProjectCommandAck, type ProjectCommandStatus, type ProfileReferenceDocuments } from './backend';
import { jsonWrite, putAsset, readAsset, routeKey } from './service';
import type { NoticeOwner } from '../api/access';
import { captureProjectWire, prepareOnce, projectJson as serviceJson, projectWrite } from './project-wire';
interface ProjectRecord { meta: ProjectMeta; doc: StoredDocBase; jobReferences?: unknown }
const projectPath = (id: string) => `/api/projects/${routeKey(id)}`;
const pendingSaves = new Map<string, Promise<{ request: RequestInit; savedAt: number }>>();
async function required<T>(path: string, init?: RequestInit, owner?: NoticeOwner): Promise<T> {
  const value = await serviceJson<T>(path, init, false, owner);
  if (value === null) throw new Error('Oops! The server didn\'t return any data for this project.');
  return value;
}
const metadata = async (id: string, archived = false) => {
  if (!archived) {
    const record = await serviceJson<ProjectRecord>(projectPath(id), undefined, true);
    if (!record) throw new Error('Oops! We couldn\'t find that project.');
    return record.meta;
  }
  return (await required<ProjectRecord>(projectPath(id) + '?archived=1')).meta;
};
type Command = { id: string; kind: ProjectCommandStatus['kind']; name?: string; newId?: string; expectedRevision?: number };
interface PendingCommand {
  command: Readonly<Command>; status: ProjectCommandStatus; request: RequestInit | null;
  running: Promise<ProjectCommandAck | ProjectMeta> | null; result: ProjectCommandAck | ProjectMeta | null;
}
const commands = new Map<string, PendingCommand>(), commandListeners = new Set<() => void>();
let commandSnapshot: readonly Readonly<ProjectCommandStatus>[] = [];
function publishCommands() {
  commandSnapshot = Object.freeze([...commands.values()].map((entry) => Object.freeze({ ...entry.status })));
  commandListeners.forEach((listener) => listener());
}
async function alreadyRestored(entry: PendingCommand, error: unknown): Promise<ProjectCommandAck> {
  const { status, code } = error as { status?: number; code?: string };
  if (entry.command.kind !== 'restore' || status !== 409 || code !== 'project_not_archived' && code !== 'revision_conflict') throw error;
  const live = await metadata(entry.command.id).catch(() => null);
  if (!Number.isSafeInteger(live?.revision)) throw error;
  entry.status.expectedRevision = live!.revision! - 1;
  return { operationId: entry.status.operationId, revision: live!.revision! };
}
function executeCommand(entry: PendingCommand): Promise<ProjectCommandAck | ProjectMeta> {
  if (entry.result) return Promise.resolve(entry.result);
  if (entry.running) return entry.running;
  const task = Promise.resolve().then(async () => {
    for (const older of commands.values()) {
      if (older === entry) break;
      if (older.command.id === entry.command.id) await executeCommand(older);
    }
    if (!entry.request) {
      const { id, kind, name, newId, expectedRevision } = entry.command;
      const revision = expectedRevision ?? (await metadata(id, kind === 'restore')).revision ?? 0;
      if (!Number.isSafeInteger(revision) || revision < 0) throw new Error('Oops! Project version got out of sync.');
      entry.status.expectedRevision = revision;
      entry.request = Object.freeze(jsonWrite('POST', { operationId: entry.status.operationId, expectedRevision: revision,
        ...(name !== undefined ? { name } : {}), ...(newId !== undefined ? { newId } : {}) }));
    }
    entry.status.state = 'sending'; entry.status.error = null; publishCommands();
    const ack = await required<ProjectCommandAck | ProjectMeta>(projectPath(entry.command.id) + '/' + entry.command.kind, entry.request)
      .catch((error: unknown) => alreadyRestored(entry, error));
    if (entry.command.kind === 'duplicate') {
      if (!('id' in ack) || ack.id !== entry.command.newId || ack.revision !== 1) throw new Error('Oops! The server returned an unexpected response while duplicating the project.');
    } else if (!('operationId' in ack) || ack.operationId !== entry.status.operationId || ack.revision !== entry.status.expectedRevision! + 1)
      throw new Error('Oops! The server returned an unexpected response while updating the project.');
    entry.result = ack; entry.status.state = 'acknowledged'; publishCommands();
    return ack;
  }).catch((error: unknown) => {
    const e = error as { status?: number; code?: string };
    entry.status.held = e?.code === 'lease_held' || e?.code === 'lease_lost';
    entry.status.state = e?.status === 409 && !entry.status.held ? 'conflict' : 'failed';
    entry.status.error = error instanceof Error ? error.message : 'Oops! Couldn\'t update project'; publishCommands(); throw error;
  });
  entry.running = task;
  void task.finally(() => { if (entry.running === task) entry.running = null; }).catch(() => {});
  return task;
}
async function projectCommand(command: Command) {
  const key = JSON.stringify(command);
  let entry = commands.get(key);
  if (!entry) {
    entry = { command: Object.freeze({ ...command }), request: null, running: null, result: null,
      status: { ...command, operationId: newKey(), expectedRevision: command.expectedRevision ?? null, state: 'preparing', error: null } };
    commands.set(key, entry); publishCommands();
  }
  const ack = await executeCommand(entry);
  if (commands.get(key) === entry) { commands.delete(key); publishCommands(); }
  return ack;
}
export const serviceStore: ProjectStore = {
  kind: 'service',
  list: () => required<ProjectMeta[]>('/api/projects'),
  load: (id) => serviceJson<ProjectRecord>(projectPath(id), undefined, true),
  async save(id, meta, doc, thumbnail) {
    const capture = await captureProjectWire({ doc, name: meta.name, objects: meta.objects });
    let revision = meta.revision ?? 0;
    for (const [prior, preparation] of [...pendingSaves]) {
      if (!prior.startsWith(`${id}/${revision}/`) || prior === `${id}/${revision}/${capture.digest}`) continue;
      const request = await preparation.then((p) => p.request, () => null);
      const replay = request && await required<{revision:number}>(projectPath(id), request, 'project').catch(() => null);
      if (pendingSaves.get(prior) === preparation) pendingSaves.delete(prior);
      if (replay?.revision === revision + 1) { revision = replay.revision; break; }
    }
    const key = `${id}/${revision}/${capture.digest}`;
    const preparation = prepareOnce(pendingSaves, key, async () => {
      const previous = revision > 0 ? await required<ProjectRecord>(projectPath(id)) : null;
      if (previous && (previous.meta.revision ?? 0) !== revision) throw Object.assign(new Error('Heads up: this project was updated elsewhere. Reload the latest version or save your current edits.'), { name: 'ProjectConflictError' });
      const record = { ...previous, meta: { ...previous?.meta, ...meta, revision, id, thumbnail: thumbnail === undefined ? previous?.meta.thumbnail ?? null : thumbnail }, doc: null };
      return { request: projectWrite(await capture.project({ operationId: newKey(), expectedRevision: revision, record })), savedAt: meta.savedAt };
    });
    let pending: Awaited<typeof preparation>;
    try { pending = await preparation; } finally { capture.release(); }
    const ack = await required<{revision:number}>(projectPath(id), pending.request, 'project');
    if (ack.revision !== revision + 1) throw new Error('Oops! Couldn\'t verify the saved project version.');
    if (pendingSaves.get(key) === preparation) pendingSaves.delete(key);
    return { revision: ack.revision, savedAt: pending.savedAt };
  },
  rename: async (id, name) => await projectCommand({ id, kind: 'rename', name }) as ProjectCommandAck,
  duplicate: async (id, newId, name) => await projectCommand({ id, kind: 'duplicate', newId, name }) as ProjectMeta,
  remove: async (id) => await projectCommand({ id, kind: 'archive' }) as ProjectCommandAck,
  restore: async (id, expectedRevision) => await projectCommand({ id, kind: 'restore', ...(expectedRevision !== undefined ? { expectedRevision } : {}) }) as ProjectCommandAck,
  commands: () => commandSnapshot,
  subscribeCommands(listener) { commandListeners.add(listener); return () => { commandListeners.delete(listener); }; },
  discardRejectedCommand(operationId) {
    const found = [...commands].find(([, entry]) => entry.status.operationId === operationId);
    if (!found || found[1].status.state !== 'conflict' && !found[1].status.held || found[1].running) throw new Error('Wait until the save conflict finishes checking before discarding changes.');
    commands.delete(found[0]); publishCommands();
  },
  referenceDocuments: () => required<ProfileReferenceDocuments>('/api/projects/references'),
  async putBlob(_id, key, data) { await putAsset(key, data); },
  getBlob: (_id, key) => readAsset(key),
};
