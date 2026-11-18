import { sessionId } from '../api/session';
import { createMachine, useMachine } from './fsm';

export interface LeaseHolder { label: string; since: number; lastSeen: number }
export type LeaseState = 'idle' | 'requesting' | 'editing' | 'viewing' | 'lost';
interface LeaseContext { id: string | null; take: boolean; holder: LeaseHolder | null; heartbeatMs: number }
type LeaseEvent =
  | { type: 'OPEN'; id: string } | { type: 'CLOSE' } | { type: 'TAKE' } | { type: 'RETRY'; id: string }
  | { type: 'GRANTED'; id: string; heartbeatMs?: number } | { type: 'HELD'; id: string; holder: LeaseHolder } | { type: 'LOST'; id: string; holder: LeaseHolder };

const path = (id: string) => `/api/project-leases/${encodeURIComponent(id)}`;
export function deviceLabel(ua = navigator.userAgent): string {
  const browser = /Edg\//.test(ua) ? 'Edge' : /Firefox\//.test(ua) ? 'Firefox' : /Chrome\//.test(ua) ? 'Chrome' : /Safari\//.test(ua) ? 'Safari' : 'Browser';
  const os = /iPad/.test(ua) ? 'iPad' : /iPhone/.test(ua) ? 'iPhone' : /Android/.test(ua) ? 'Android' : /Mac OS X/.test(ua) ? 'macOS'
    : /Windows/.test(ua) ? 'Windows' : /Linux/.test(ua) ? 'Linux' : '';
  return os ? `${browser} on ${os}` : browser;
}

let inflight = false, queued: { id: string; take: boolean } | null = null, retryTimer: ReturnType<typeof setTimeout> | undefined;
async function request(id: string, take: boolean) {
  if (inflight) { queued = { id, take: take || (queued?.id === id && queued.take) }; return; }
  inflight = true; clearTimeout(retryTimer); retryTimer = undefined;
  const same = (e: LeaseEvent) => lease.send(e);
  try {
    const response = await fetch(path(id), { method: 'POST', cache: 'no-store', headers: { 'Content-Type': 'application/json', 'X-Orca-Session': sessionId },
      body: JSON.stringify({ session: sessionId, label: deviceLabel(), take }), signal: AbortSignal.timeout(10_000) });
    const body = await response.json().catch(() => ({}));
    if (response.ok || body.code === 'project_missing') return same({ type: 'GRANTED', id, heartbeatMs: body.heartbeatMs });
    if (body.code === 'lease_held' || body.code === 'lease_lost') return same({ type: body.code === 'lease_lost' ? 'LOST' : 'HELD', id, holder: body.holder });
    throw new Error(`Session lock (${response.status})`);
  } catch {
    retryTimer = setTimeout(() => { retryTimer = undefined; lease.send({ type: 'RETRY', id }); }, 5_000);
  } finally {
    inflight = false;
    const next = queued; queued = null;
    if (next) void request(next.id, next.take);
  }
}
const release = (id: string | null) => {
  if (id) void fetch(`${path(id)}/release`, { method: 'POST', keepalive: true, body: JSON.stringify({ session: sessionId }) }).catch(() => {});
};
const mine = (c: LeaseContext, e: { id: string }) => c.id === e.id;
const opened = { target: 'requesting', action: (c: LeaseContext, e: { id: string }) => { if (c.id !== e.id) release(c.id); return { id: e.id, take: false, holder: null }; } } as const;
const closed = { target: 'idle', action: (c: LeaseContext) => { release(c.id); return { id: null, holder: null }; } } as const;
const granted = { target: 'editing', guard: mine, action: (_: LeaseContext, e: Extract<LeaseEvent, { type: 'GRANTED' }>) => ({ holder: null, take: false, ...(e.heartbeatMs ? { heartbeatMs: e.heartbeatMs } : {}) }) } as const;
const holderOf = (_: LeaseContext, e: { holder: LeaseHolder }) => ({ holder: e.holder });

export const lease = createMachine<LeaseState, LeaseEvent, LeaseContext>({
  id: 'project-lease', initial: 'idle', strict: true,
  context: { id: null, take: false, holder: null, heartbeatMs: 15_000 },
  states: {
    idle: { on: { OPEN: opened, CLOSE: {}, TAKE: {}, RETRY: {}, GRANTED: {}, HELD: {}, LOST: {} } },
    requesting: {
      entry: (c) => { if (c.id) void request(c.id, c.take); },
      on: { OPEN: opened, CLOSE: closed, TAKE: { action: () => ({ take: true }) }, RETRY: { target: 'requesting', guard: mine },
        GRANTED: granted, HELD: { target: 'viewing', guard: mine, action: holderOf }, LOST: { target: 'lost', guard: mine, action: holderOf } } },
    editing: {
      after: { ms: (c) => c.heartbeatMs, target: 'editing', action: (c) => { if (c.id) void request(c.id, false); } },
      on: { OPEN: opened, CLOSE: closed, TAKE: {}, RETRY: { guard: mine, action: (c) => { if (c.id) void request(c.id, false); } },
        GRANTED: { guard: mine }, HELD: { target: 'viewing', guard: mine, action: holderOf }, LOST: { target: 'lost', guard: mine, action: holderOf } } },
    viewing: {
      after: { ms: (c) => c.heartbeatMs, target: 'requesting' },
      on: { OPEN: opened, CLOSE: closed, TAKE: { target: 'requesting', action: () => ({ take: true }) }, RETRY: {},
        GRANTED: granted, HELD: { guard: mine, action: holderOf }, LOST: { guard: mine, action: holderOf } } },
    lost: {
      on: { OPEN: opened, CLOSE: closed, TAKE: { target: 'requesting', action: () => ({ take: true }) }, RETRY: {},
        GRANTED: {}, HELD: { guard: mine, action: holderOf }, LOST: { guard: mine, action: holderOf } } },
  },
});

export const leaseWritable = () => lease.get().value === 'editing' || lease.get().value === 'idle';
export const leaseSettled = (): Promise<void> => lease.get().value !== 'requesting' ? Promise.resolve()
  : new Promise((resolve) => { const stop = lease.subscribe((s) => { if (s.value !== 'requesting') { stop(); resolve(); } }); });
export const takeControl = () => lease.send({ type: 'TAKE' });
export async function acquireLease(id: string): Promise<boolean> {
  if (lease.get().context.id !== id || lease.get().value === 'idle') lease.send({ type: 'OPEN', id });
  await leaseSettled();
  return leaseWritable();
}
export const useProjectLease = () => useMachine(lease);
export function leaseRefused(error: unknown, id: string): boolean {
  const e = error as { code?: string; message?: string };
  if (e?.code !== 'lease_held' && e?.code !== 'lease_lost') return false;
  lease.send({ type: e.code === 'lease_lost' ? 'LOST' : 'HELD', id, holder: lease.get().context.holder ?? { label: '', since: 0, lastSeen: 0 } });
  if (lease.get().value === 'editing' || lease.get().value === 'idle') lease.send({ type: 'OPEN', id });
  return true;
}

export function followProjectLease(current: () => string | null, subscribe: (f: () => void) => () => void, onWritable: (from: LeaseState, id: string) => void) {
  let shown: string | null = null;
  const sync = () => {
    const id = current();
    if (id === shown) return;
    shown = id;
    if (id && lease.get().context.id === id && lease.get().value !== 'idle') return;
    lease.send(id ? { type: 'OPEN', id } : { type: 'CLOSE' });
  };
  let was = leaseWritable(), before: LeaseState = lease.get().value, readOnly: LeaseState = before;
  lease.subscribe((s) => {
    const now = leaseWritable();
    if (s.value === 'viewing' || s.value === 'lost') readOnly = s.value;
    else if (s.value === 'requesting' && (before === 'idle' || before === 'editing')) readOnly = 'idle';
    if (now && !was && s.context.id) onWritable(readOnly, s.context.id);
    was = now; before = s.value;
  });
  window.addEventListener('pagehide', () => {
    const { value, context } = lease.get();
    if (value === 'editing' && context.id) navigator.sendBeacon(`${path(context.id)}/release`, JSON.stringify({ session: sessionId }));
  });
  sync();
  return subscribe(sync);
}
