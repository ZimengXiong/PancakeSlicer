import { useSyncExternalStore } from 'react';
import { refreshProjects } from '../flow/project';
import { projectId } from '../persist/identity';
import { jsonWrite, serviceJson } from '../persist/service';

export const FOLDER_NAME_MAX = 120;

export interface Folder { id: string; name: string; parentId?: string; revision: number; createdAt: number; updatedAt: number }
export type FoldersSnapshot = { state: 'loading' | 'ready' | 'failed'; revision: number; folders: readonly Folder[]; error: string | null };

let snapshot: FoldersSnapshot = { state: 'loading', revision: 0, folders: [], error: null };
let loading: Promise<void> | null = null;
const listeners = new Set<() => void>();
const publish = (next: FoldersSnapshot) => { snapshot = next; listeners.forEach((listener) => listener()); };
const message = (error: unknown) => error instanceof Error ? error.message : String(error);

let reads = 0;
export function loadFolders(fresh = false): Promise<void> {
  if (loading && !fresh) return loading;
  const mine = ++reads;
  const read: Promise<void> = serviceJson<{ revision: number; folders: Folder[] }>('/api/folders').then((remote) => {
    if (!remote || !Number.isSafeInteger(remote.revision) || !Array.isArray(remote.folders)) throw new Error('Oops! Couldn\'t load your folders.');
    if (mine === reads) publish({ state: 'ready', revision: remote.revision, folders: remote.folders, error: null });
  }).catch((error: unknown) => {
    if (mine === reads) publish({ ...snapshot, state: 'failed', error: message(error) });
  }).finally(() => { if (loading === read) loading = null; });
  return (loading = read);
}

export function useFolders(): FoldersSnapshot {
  return useSyncExternalStore((listener) => {
    listeners.add(listener);
    if (snapshot.state === 'loading' && !loading) void loadFolders();
    return () => { listeners.delete(listener); };
  }, () => snapshot);
}

let writes: Promise<unknown> = Promise.resolve();
function write(method: 'POST' | 'PATCH', path: string, body: Record<string, unknown>, projects = false): Promise<void> {
  const run = writes.then(() => writeOnce(method, path, body, projects));
  writes = run.catch(() => {});
  return run;
}
async function writeOnce(method: 'POST' | 'PATCH', path: string, body: Record<string, unknown>, projects: boolean): Promise<void> {
  try {
    for (let attempt = 0; ; attempt++) {
      if (snapshot.state !== 'ready') await loadFolders();
      if (snapshot.state !== 'ready') throw new Error(snapshot.error ?? 'Folders aren\'t available right now.');
      try {
        await serviceJson(path, jsonWrite(method, { operationId: projectId(), expectedRevision: snapshot.revision, ...body }));
        break;
      } catch (error) {
        if ((error as { code?: string }).code !== 'revision_conflict' || attempt > 0) throw error;
        await loadFolders(true);
      }
    }
  } finally {
    await loadFolders(true);
    if (projects) await refreshProjects();
  }
}

export async function createFolder(name: string, parentId: string | null): Promise<string> {
  const id = projectId();
  await write('POST', '/api/folders', { id, name, parentId });
  return id;
}
export const renameFolder = (id: string, name: string) => write('PATCH', `/api/folders/${encodeURIComponent(id)}`, { name });
export const moveFolder = (id: string, parentId: string | null) => write('PATCH', `/api/folders/${encodeURIComponent(id)}`, { parentId });
export const deleteFolder = (id: string) => write('POST', `/api/folders/${encodeURIComponent(id)}/delete`, {}, true);
export const moveProjects = (projectIds: readonly string[], folderId: string | null) => write('POST', '/api/folders/move', { projectIds, folderId }, true);

export const childFolders = (folders: readonly Folder[], parent: string | null) =>
  folders.filter((f) => (f.parentId ?? null) === parent).sort((a, b) => a.name.localeCompare(b.name, undefined, { numeric: true, sensitivity: 'base' }));

export function folderPath(folders: readonly Folder[], id: string | null): Folder[] {
  const byId = new Map(folders.map((f) => [f.id, f])), path: Folder[] = [];
  for (let f = id ? byId.get(id) : undefined; f && path.length < 32; f = f.parentId ? byId.get(f.parentId) : undefined) path.unshift(f);
  return path;
}

export function folderTree(folders: readonly Folder[]): { folder: Folder; depth: number }[] {
  const out: { folder: Folder; depth: number }[] = [];
  const visit = (parent: string | null, depth: number) => {
    for (const folder of childFolders(folders, parent)) { out.push({ folder, depth }); if (depth < 16) visit(folder.id, depth + 1); }
  };
  visit(null, 0);
  return out;
}
