import { createServiceDocument, type KeyMerge } from './service-document';
import { mergeSetups, type BlankSetup } from './blank-setup';
import { requireWorkspaceService } from './service';
const latest = (_remote: unknown, local: unknown) => local;
const setups = (remote: unknown, local: unknown) => mergeSetups(remote as BlankSetup | null, local as BlankSetup | null);
type Notices = Record<string, { at?: number; dismissed?: boolean }>;
const notices = (remote: unknown, local: unknown) => {
  const all: Notices = { ...(remote as Notices ?? {}) };
  for (const [id, n] of Object.entries(local as Notices ?? {})) if ((n?.at ?? 0) >= (all[id]?.at ?? 0)) all[id] = n;
  return Object.fromEntries(Object.entries(all).sort(([, a], [, b]) => (b?.at ?? 0) - (a?.at ?? 0)).slice(0, 24));
};
export type SessionDocument = { fsmRecovery?: unknown; blankSetup?: BlankSetup | null; projectSlotNotices?: Notices };
const merge: Record<keyof SessionDocument, KeyMerge> = { fsmRecovery: latest, blankSetup: setups, projectSlotNotices: notices };
const session = createServiceDocument<SessionDocument>('/api/session', 'PATCH', 'session', requireWorkspaceService, merge);
export const serviceSessionSnapshot = session.snapshot;
export const subscribeServiceSession = session.subscribe;
export const sessionDocument = () => session.snapshot().document;
export const loadServiceSession = session.hydrate;
export function patchServiceSession(document: SessionDocument): void { session.patch(structuredClone(document)); }
export const retryServiceSession = () => session.flush(true);
export const flushServiceSession = session.flush;
export const sessionSettled = (): Promise<void> => new Promise((resolve) => {
  const settled = () => !['debounced', 'writing'].includes(session.snapshot().state);
  if (settled()) return resolve();
  const stop = session.subscribe(() => { if (settled()) { stop(); resolve(); } });
});
export const reloadServiceSession = session.reload;
export const serviceSessionUnreachable = () => session.machine.get().context.transient;
