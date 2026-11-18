import * as project from '../flow/project';
import { setState, toast } from '../state/store';

import { useMemo } from 'react';
import type { ProjectMeta } from '../flow/project';

export { flushProject, storing, type ProjectMeta } from '../flow/project';

export type ListedProject = ProjectMeta & { folderId: string | null };

export const clean = (p: Partial<ProjectMeta> | null | undefined): ListedProject | null => (!p || typeof p.id !== 'string' ? null : {
  id: p.id,
  name: typeof p.name === 'string' && p.name.trim() ? p.name : 'Untitled',
  savedAt: Number.isFinite(p.savedAt) ? Number(p.savedAt) : 0,
  ...(Number.isSafeInteger(p.revision) ? { revision: p.revision } : {}),
  objects: Number.isFinite(p.objects) ? Number(p.objects) : 0,
  thumbnail: typeof p.thumbnail === 'string' && (p.thumbnail.startsWith('data:image/') || p.thumbnail.startsWith('/api/project-thumbnails/')) ? p.thumbnail : null,
  folderId: typeof (p as ListedProject).folderId === 'string' ? (p as ListedProject).folderId : null,
});

export function useProjects(): ListedProject[] | null {
  const list = project.useProjects();
  return useMemo(() => (Array.isArray(list) ? list.map(clean).filter((p): p is ListedProject => !!p) : null), [list]);
}

export async function listTrash(): Promise<ListedProject[]> {
  const { projectJson } = await import('../persist/project-wire');
  const rows = await projectJson<Partial<ProjectMeta>[]>('/api/projects?archived=1');
  if (!Array.isArray(rows)) throw new Error('Oops! Couldn\'t load the Trash folder.');
  return rows.map(clean).filter((p): p is ListedProject => !!p);
}

async function guard<T>(failed: 'project_new_failed' | 'project_open_failed' | 'project_rename_failed' | 'project_duplicate_failed' | 'project_delete_failed', f: () => Promise<T>, fallback: T): Promise<T> {
  try {
    return await f();
  } catch (e) {
    console.warn(failed, e);
    toast(failed);
    return fallback;
  }
}

export const newProject = () => guard('project_new_failed', async () => {const created=await project.newProject();if(created)setState({tab:'prepare'});return created;},false);
export const openProject = (id: string) => guard('project_open_failed', async () => { await project.openProject(id); setState({ tab: 'prepare' }); }, undefined);
export const renameProject = (id: string, name: string) => guard('project_rename_failed', () => project.renameProject(id, name), undefined);
export const duplicateProject = (id: string) => guard('project_duplicate_failed', () => project.duplicateProject(id), null);
export const deleteProject = (id: string) => guard('project_delete_failed', () => project.deleteProject(id), null);
