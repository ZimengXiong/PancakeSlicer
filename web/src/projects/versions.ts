import { documentStrip } from '../profiles/slots';
import { useSyncExternalStore } from 'react';
import type { StoredDocBase } from '../persist/backend';
import { plateSignature } from '../slice-input';
import type { Doc } from '../state/history';
import { getState, subscribeStore, toast, type JobView, type State } from '../state/store';
import { slotColour } from '../profiles/filament-colour';
import type { JobStatus } from '../api/types';
import { project, flushProject, leaveProjectVersion, restoreProjectVersion, viewProjectVersion, type ProjectMeta } from '../flow/project';
import { versionShown, versionView, versionWanted } from '../flow/version-view';
import { changeParts, describeChange, profileName, type VersionChange } from './version-change';

export type SendState = 'sending' | 'received' | 'printing' | 'printed' | 'failed' | 'print_failed';
export interface VersionSend { id: string; printer: string; name: string; at: number; state: SendState; end?: number; error?: string }
export interface ProjectVersion {
  id: number; revision: number; at: number; until: number; record: string; name: string; objects: number;
  change: VersionChange[]; sealed?: boolean; sends?: VersionSend[];
  slice?: { at: number; timeS?: number; grams?: number; layers?: number; mm?: number; supportG?: number };
}
export interface HistoryState {
  open: boolean;
  projectId: string | null;
  versions: ProjectVersion[] | null;
  error: string | null;
  viewing: ProjectVersion | null;
  loading: number | null;
}

let state: HistoryState = { open: false, projectId: null, versions: null, error: null, viewing: null, loading: null };
const listeners = new Set<() => void>();
const set = (patch: Partial<HistoryState>) => { state = { ...state, ...patch }; listeners.forEach((l) => l()); };
export const historyState = () => state;
export const useHistory = () => useSyncExternalStore((l) => (listeners.add(l), () => void listeners.delete(l)), historyState);

const service = () => import('../persist/service');
const path = (id: string) => `/api/versions/${encodeURIComponent(id)}`;
async function post<T>(url: string, body: unknown): Promise<T> {
  const { serviceJson, jsonWrite } = await service();
  const value = await serviceJson<T>(url, jsonWrite('POST', body));
  if (value === null) throw new Error('Oops! Couldn\'t load the version details from the server.');
  return value;
}
const errorText = (e: unknown) => e instanceof Error ? e.message : String(e);

const saved = new Map<string, Doc>();
let override: { id: string; change: VersionChange[] } | null = null;
export const savedVersionBase = (id: string, doc: Doc) => { saved.set(id, doc); };
export function changeForSave(id: string, doc: Doc, revision: number): VersionChange[] {
  if (override?.id === id) { const { change } = override; override = null; return change; }
  const base = saved.get(id);
  return base ? describeChange(base, doc) : revision === 0 ? [{ label: 'Created' }] : [];
}

let reading = 0;
export async function refreshHistory(id = project.get().context.id) {
  const mine = ++reading;
  if (state.projectId !== id) set({ projectId: id, versions: null, error: null });
  try {
    const { serviceJson } = await service();
    const list = await serviceJson<{ revision: number; versions: ProjectVersion[] }>(path(id), undefined, true);
    if (mine === reading) set({ versions: (list?.versions ?? []).map((v) => ({ ...v, change: Array.isArray(v.change) ? v.change : [] })), error: null });
  } catch (e) {
    if (mine === reading) set({ error: errorText(e) });
  }
}
export function openHistory() {
  const { value, context } = project.get();
  if (value === 'blank' || value === 'missing') return;
  set({ open: true });
  void refreshHistory(context.id);
}
export function closeHistory() { set({ open: false }); }
export const toggleHistory = () => (state.open ? closeHistory() : openHistory());

async function showSlice(id: string, v: ProjectVersion, request: number) {
  try {
    const status = await post<JobStatus & { id: string }>(`${path(id)}/${v.id}/open-slice`, {});
    const job: JobView = { id: status.id, state: 'done', pct: 100, stage: 'done', status, plateSignature: plateSignature(getState()), warnings: [] };
    versionView.send({ type: 'SLICE', request, job });
  } catch (e) {
    const current = versionView.get().context.request === request;
    versionView.send({ type: 'SLICE_FAIL', request });
    if (current) toast('couldnt_load_slice_preview', { detail: errorText(e) });
  }
}

versionView.subscribe(({ value, context }) => set({ viewing: context.shown, loading: (value === 'opening' ? context.target : value === 'loading-slice' ? context.shown : null)?.id ?? null }));
export async function viewVersion(v: ProjectVersion) {
  const id = state.projectId ?? project.get().context.id, latest = state.versions?.[0];
  if (latest && v.id === latest.id) return backToLatest();
  versionView.send({ type: 'VIEW', version: v });
  const request = versionView.get().context.request;
  try {
    const { projectJson } = await import('../persist/project-wire');
    const record = await projectJson<{ meta: ProjectMeta; doc: StoredDocBase }>(`${path(id)}/${v.id}`);
    if (!record) throw new Error('That older version is no longer stored on the server.');
    if (!versionWanted(request)) return;
    if (!(await viewProjectVersion(id, record, request))) return void versionView.send({ type: 'FAIL', request });
    if (v.slice) await showSlice(id, v, request);
  } catch (e) {
    const wanted = versionWanted(request);
    versionView.send({ type: 'FAIL', request });
    if (wanted) toast('versions_failed', { detail: errorText(e) });
  }
}
export async function backToLatest() {
  const id = state.projectId, shown = versionShown();
  versionView.send({ type: 'LATEST' });
  if (id && shown) await leaveProjectVersion(id);
}
export async function restoreVersion() {
  const v = versionShown(), id = state.projectId;
  if (!v || !id) return;
  override = { id, change: [{ key: `restore:${v.until}`, label: 'Restored' }] };
  try {
    await flushProject();
    await post(`${path(id)}/seal`, { revision: project.get().context.revision });
    await restoreProjectVersion();
    await attachSlice(finishedJob());
  } catch (e) {
    toast('versions_failed_2', { detail: errorText(e) });
  }
  void refreshHistory(id);
}

async function sealCurrent(signature: string): Promise<{ id: string; revision: number; version: number } | null> {
  await flushProject();
  const { value, context } = project.get();
  if (state.viewing || value !== 'saved' || plateSignature(getState()) !== signature) return null;
  const sealed = await post<ProjectVersion>(`${path(context.id)}/seal`, { revision: context.revision });
  return { id: context.id, revision: context.revision, version: sealed.id };
}
let sealing: { signature: string; done: ReturnType<typeof sealCurrent> } | null = null;
const finishedJob = (job: JobView = getState().job) =>
  job.state === 'done' && job.id && !job.stale && !job.calib && job.plateSignature && job.plateSignature === plateSignature(getState()) ? job : null;
async function attachSlice(job: JobView | null) {
  if (!job?.id || !job.plateSignature || state.viewing) return;
  const seal = sealing?.signature === job.plateSignature ? await sealing.done.catch(() => null) : null;
  const target = seal && seal.revision === project.get().context.revision ? seal : await sealCurrent(job.plateSignature);
  if (!target || plateSignature(getState()) !== job.plateSignature) return;
  await post(`${path(target.id)}/${target.version}/slice`, { revision: target.revision, jobId: job.id });
  if (state.open && state.projectId === target.id) void refreshHistory(target.id);
}

let tracking = false;
export function startVersionTracking() {
  if (tracking) return;
  tracking = true;
  let started: string | undefined, attached: string | undefined;
  subscribeStore(() => {
    const job = getState().job;
    if (state.viewing || job.calib || !job.plateSignature) return;
    if ((job.state === 'submitting' || job.state === 'running') && job.plateSignature !== started) {
      started = job.plateSignature;
      const done = sealCurrent(started);
      done.catch(() => {});
      sealing = { signature: started, done };
    }
    const finished = finishedJob(job);
    if (finished && finished.id !== attached) {
      attached = finished.id;
      void attachSlice(finished).catch((e: unknown) => console.warn('Version slice not kept', e));
    }
  });
  let sends = '';
  subscribeStore(() => {
    const now = getState().printerConnections.transfers.map((t) => `${t.id}:${t.state}:${t.outcome ?? ''}:${t.job_end ?? ''}`).join();
    if (now === sends) return;
    sends = now;
    if (state.open && state.projectId) void refreshHistory(state.projectId);
  });
  let revision = project.get().context.revision, id = project.get().context.id;
  project.subscribe((snap) => {
    if (snap.value === 'opening' || snap.context.id !== id) versionView.send({ type: 'LATEST' });
    if (snap.context.id !== id) {
      id = snap.context.id;
      if (state.projectId !== id) set({ versions: null, projectId: null, open: state.open && snap.value !== 'blank' });
      if (state.open) void refreshHistory(id);
    } else if (snap.context.revision !== revision && state.open) void refreshHistory(id);
    revision = snap.context.revision;
  });
}

const clock = (t: number) => new Date(t).toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' });
export function dayOf(t: number, now = Date.now()) {
  const d = new Date(t), today = new Date(now);
  const days = Math.round((new Date(today.toDateString()).getTime() - new Date(d.toDateString()).getTime()) / 86_400_000);
  return days === 0 ? 'Today' : days === 1 ? 'Yesterday' : d.toLocaleDateString([], { weekday: days < 7 ? 'long' : undefined, month: 'short', day: 'numeric', year: d.getFullYear() === today.getFullYear() ? undefined : 'numeric' });
}
export const withSeconds = (t: number) => new Date(t).toLocaleTimeString([], { hour: 'numeric', minute: '2-digit', second: '2-digit' });
export function rowTimes(versions: readonly ProjectVersion[], now = Date.now()): Map<number, string> {
  const base = versions.map((v) => {
    const age = now - v.until;
    return age < 60_000 ? 'Just now' : age < 3_600_000 ? `${Math.floor(age / 60_000)} min ago` : clock(v.until);
  });
  return new Map(versions.map((v, i) => [v.id, base[i] === base[i - 1] || base[i] === base[i + 1] ? withSeconds(v.until) : base[i]]));
}
export const versionTime = (t: number) => dayOf(t) === 'Today' ? clock(t) : `${dayOf(t)} ${clock(t)}`;
export const clockTime = clock;
export const shortDate = (t: number) => new Date(t).toLocaleDateString([], { month: 'short', day: 'numeric', year: new Date(t).getFullYear() === new Date().getFullYear() ? undefined : 'numeric' });

export const changeLine = (c: VersionChange) => {
  const at = c.key?.startsWith('restore:') ? Number(c.key.slice(8)) : NaN;
  return Number.isFinite(at) ? { name: 'Restored', to: withSeconds(at) } : changeParts(c);
};

export interface VersionSetup { printer: string; process: string; filaments: { name: string; colour: string }[]; picture?: string }
const setups = new Map<string, Promise<VersionSetup>>();
export function versionSetup(id: string, v: ProjectVersion): Promise<VersionSetup> {
  const key = `${id}/${v.id}/${v.record}`;
  let found = setups.get(key);
  if (!found) {
    found = import('../persist/project-wire').then(({ projectJson }) => projectJson<{ meta?: { thumbnail?: unknown }; doc: Partial<State> }>(`${path(id)}/${v.id}/summary`))
      .then((summary) => {
        const doc = summary?.doc ?? {}, picture = summary?.meta?.thumbnail;
        return { printer: profileName('machine', doc.printer), process: profileName('process', doc.process),
          filaments: (({ filaments, colours } = documentStrip(doc) ?? { filaments: [], colours: [] }) => filaments.map((k, i) => ({ name: profileName('filament', k), colour: colours[i] ?? slotColour(getState(), i) })))(),
          picture: typeof picture === 'string' && picture.startsWith('data:image/') ? picture : undefined };
      });
    found.catch(() => setups.delete(key));
    setups.set(key, found);
  }
  return found;
}
export const prefetchSetup = (id: string, v: ProjectVersion) => void versionSetup(id, v).catch(() => {});

const reached: Record<SendState, number> = { failed: 0, sending: 1, received: 2, print_failed: 3, printing: 4, printed: 5 };
export const furthestSend = (sends: readonly VersionSend[]) => sends.reduce((a, b) => (reached[b.state] >= reached[a.state] ? b : a));
export const sendsLive = (versions: readonly ProjectVersion[] | null, now = Date.now()) =>
  !!versions?.some((v) => v.sends?.some((s) => s.state === 'sending' || s.state === 'printing' || s.state === 'received' && now - s.at < 600_000));
const sendWhat: Record<SendState, (name: string) => string> = {
  sending: (n) => `Sending to ${n}`, received: (n) => `Sent to ${n}`, printing: (n) => `Printing on ${n}`,
  printed: (n) => `Printed on ${n}`, failed: (n) => `Send to ${n} failed`, print_failed: (n) => `Print on ${n} failed`,
};
export function sendLabel(s: VersionSend) {
  const t = s.state === 'printed' || s.state === 'print_failed' ? s.end ?? s.at : s.at;
  return `${sendWhat[s.state](s.name.replace(/^Bambu Lab /, ''))} ${dayOf(t) === 'Today' ? '' : `${shortDate(t)} `}at ${clock(t)}`;
}
