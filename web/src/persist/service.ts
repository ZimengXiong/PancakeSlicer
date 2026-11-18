import { mutation, responseError, type NoticeOwner } from '../api/access';
import { documentText, parseDocument } from './wire';
import { readHealth } from '../api/health';

export async function serviceRequest(path: string, init: RequestInit = {}, missing = false, owner?: NoticeOwner): Promise<Response | null> {
  try {
    const write = init.method && init.method !== 'GET' && init.method !== 'HEAD';
    const request = { cache: write ? 'no-store' as const : 'no-cache' as const, ...init, signal: init.signal ?? AbortSignal.timeout(30_000) };
    const response = write ? await mutation(path, request, false, undefined, undefined, false, owner) : await fetch(path, request);
    if (missing && response.status === 404) return null;
    if (!response.ok) throw await responseError(response);
    return response;
  } catch (error) {
    const { status, code } = (error ?? {}) as { status?: number; code?: string };
    if (missing && status === 404) return null;
    const name = code === 'revision_conflict' || code === 'draft_conflict' ? 'ProjectConflictError' : status === 410 ? 'ProjectDeletedError'
      : status === 507 ? 'QuotaExceededError' : 'ProjectServiceError';
    throw Object.assign(error instanceof Error ? error : new Error(String(error)), { name });
  }
}
export async function serviceJson<T>(path: string, init?: RequestInit, missing = false, owner?: NoticeOwner): Promise<T | null> {
  const response = await serviceRequest(path, init, missing, owner);
  return response && parseDocument<T>(await response.text());
}
export function jsonWrite(method: 'PUT' | 'POST' | 'PATCH' | 'DELETE', value: unknown): RequestInit {
  return { method, headers: { 'Content-Type': 'application/json' }, body: documentText(value) };
}
export const routeKey = (key: string) => encodeURIComponent(key);

const assets = new Map<string, Promise<unknown>>();
const assetPath = (key: string) => `/api/assets/${routeKey(key)}`;
export function putAsset(key: string, blob: Blob): Promise<unknown> {
  let sent = assets.get(key);
  if (!sent) assets.set(key, (sent = serviceRequest(assetPath(key), { method: 'PUT', body: blob }, false, 'project').then((r) => r?.text()).catch((error) => { assets.delete(key); throw error; })));
  return sent;
}
export async function readAsset(key: string): Promise<Blob | null> {
  const response = await serviceRequest(assetPath(key), undefined, true);
  if (!response) return null;
  assets.set(key, Promise.resolve());
  return response.blob();
}

let workspaceService: Promise<void> | null = null;
export function requireWorkspaceService(): Promise<void> {
  workspaceService ??= readHealth(15_000).then(async response => {
    if (!response.ok) throw Object.assign(new Error('Oops! The slicing service isn\'t reachable right now.'), { status: response.status });
    if ((await response.json()).workspace_schema !== 1)
      throw new Error('This version of the app needs an updated slicing service. Don\'t worry—your projects, drafts, and preferences are safe! Update the service and try again.');
  }).catch(error => { workspaceService = null; throw error; });
  return workspaceService;
}
