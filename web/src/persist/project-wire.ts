import { serviceRequest } from './service';
import type { NoticeOwner } from '../api/access';
export const captureProjectWire = async (value: unknown) => (await import('./wire-worker')).captureWire(value);
export async function projectJson<T>(path: string, init?: RequestInit, missing = false, owner?: NoticeOwner): Promise<T | null> {
  const response = await serviceRequest(path, init, missing, owner);
  return response && (await import('./wire-worker')).parseWire<T>(await response.text());
}
export const projectWrite = (body: string): RequestInit => ({ method: 'PUT', headers: { 'Content-Type': 'application/json' }, body });
export function prepareOnce<T>(pending: Map<string, Promise<T>>, key: string, prepare: () => Promise<T>): Promise<T> {
  let operation = pending.get(key);
  if (!operation) {
    operation = Promise.resolve().then(prepare); pending.set(key, operation);
    const captured = operation;
    void operation.catch(() => { if (pending.get(key) === captured) pending.delete(key); });
  }
  return operation;
}
