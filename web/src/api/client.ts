import { submissionHeaders } from '../flow/submission';
import { bootstrapAccessToken, failureCode, mutation, responseError } from './access';
import { attachFiles, postJob } from './blobs';
import { readHealth } from './health';
import { followJobStream, type JobStreamEvent } from './job-stream';
import type { Config, JobRequest, JobStatus, ProgressEvent } from './types';
import { DEMO, NO_ENGINE } from '../demo/config';

export interface Api {
  mock: boolean;
  submit(req: JobRequest, signal?: AbortSignal, onUpload?: (fraction: number) => void): Promise<string>;
  status(id: string): Promise<JobStatus>;
  events(id: string, on: { progress(e: ProgressEvent): void; done(s: JobStatus): void; error(msg: string, code: string): void; log?(stream: string, line: string): void; result?(s: JobStatus): void; timeoutWarning?(): void }): () => void;
  cancel(id: string): Promise<void>;
  downloadUrl(id: string): string;
  movesUrl(id: string): string;
}

async function json<T>(r: Response): Promise<T> {
  if (!r.ok) throw await responseError(r);
  return r.json() as Promise<T>;
}

export function parseProgress(data: string): ProgressEvent | null {
  try {
    const j = JSON.parse(data);
    const value = j?.pct ?? j?.percent ?? j?.progress;
    const pct = typeof value === 'number' || typeof value === 'string' && value.trim() ? Number(value) : NaN;
    if (j && typeof j === 'object' && Number.isFinite(pct)) return { stage: String(j.stage ?? j.message ?? ''), pct, message: j.message };
  } catch {
  }
  const m = /^\s*progress\s+(\S+)\s+(-?\d+(?:\.\d+)?)\s*$/.exec(data);
  return m && Number.isFinite(Number(m[2])) ? { stage: m[1], pct: Number(m[2]) } : null;
}

const statusRequests = new Map<string, Promise<JobStatus>>();

function isTerminalState(data: string) {
  try { return ['done', 'failed', 'canceled'].includes(JSON.parse(data)?.state); } catch { return false; }
}

const refusal = (code: string, message: string) => Object.assign(new Error(message), { code });

const realApi: Api = {
  mock: false,
  async submit(req, signal, onUpload) {
    if (DEMO) throw new Error(NO_ENGINE);
    const fd = new FormData();
    await attachFiles(fd, req.files, signal, onUpload);
    const { printer, print } = req.library;
    if (!printer) throw refusal('slice_refused', 'Pick a printer first to get started.');
    if (!print) throw refusal('slice_refused', 'Pick a print profile first.');
    fd.append('library', JSON.stringify({ ...req.library, project: req.project }));
    if (req.plate_index !== undefined) fd.append('plate_index', String(req.plate_index));
    if (req.plate) fd.append('plate', JSON.stringify({ ...req.plate, objects: req.plate.objects.map((o) => ({ ...o,
      ...(o.settings ? { settings: o.settings } : {}),
      ...(o.volumes ? { volumes: o.volumes.map((v) => v.settings ? { ...v, settings: v.settings } : v) } : {}),
      ...(o.ranges ? { ranges: o.ranges.map((r) => ({ ...r, settings: r.settings })) } : {}) })) }));
    if (req.project_instances) fd.append('project_instances', JSON.stringify(req.project_instances));
    if (req.events.length) fd.append('events', JSON.stringify(req.events));
    fd.append('output', req.output);
    if (req.priority) fd.append('priority', req.priority);
    if (req.project_settings) fd.append('project_settings', JSON.stringify(req.project_settings));
    if (req.project_edits) fd.append('project_edits', JSON.stringify(req.project_edits.map((e) => 'settings' in e && e.settings ? { ...e, settings: e.settings } : e)));
    if (req.force_planar) fd.append('force_planar', '1');
    if (req.floating) fd.append('floating', '1');
    if (req.allow_mixed_temperatures) fd.append('allow_mixed_temperatures', '1');
    if (req.calib) { fd.append('calib', req.calib); fd.append('calib_params', req.calib_params ?? ''); if (req.calib_head) fd.append('calib_head', req.calib_head); }
    if (req.preview_budget !== undefined) fd.append('budget', String(req.preview_budget));
    const overlay = req.preview_budget !== undefined || !!req.plan_tops;
    const url = req.plan_tops ? '/api/top-surfaces/plan' : req.preview_budget !== undefined ? '/api/patch-preview' : '/api/jobs';
    const j = await json<{ id: string }>(await postJob(fd, url, { headers: submissionHeaders(req), retry: !overlay, signal, quiet: overlay }));
    return j.id;
  },
  status(id) {
    let pending = statusRequests.get(id);
    if (!pending) {
      pending = fetch(`/api/jobs/${id}?request=0`).then((r) => json<JobStatus>(r)).finally(() => statusRequests.delete(id));
      statusRequests.set(id, pending);
    }
    return pending;
  },
  events(id, on) {
    let finished = false, disposed = false, checking = false, resulted = false, last = -1;
    let reason: string | undefined, reasonCode: string | undefined, stop = () => {}, retry: ReturnType<typeof setTimeout> | undefined;
    const close = () => { finished = true; stop(); clearTimeout(retry); };
    const finish = async (reconnect = false) => {
      if (finished || checking) return;
      checking = true;
      try {
        const s = await realApi.status(id);
        if (disposed) return;
        if (s.state === 'queued' || s.state === 'running') {
          retry = setTimeout(reconnect ? follow : () => void finish(), 1000);
          return;
        }
        close();
        if (s.state === 'done') on.done(s);
        else on.error(s.error ?? `job ${s.state}`, s.error_code ?? (s.state === 'canceled' ? 'job_canceled' : 'slice_failed'));
      } catch (e) {
        if (!disposed) {
          close();
          on.error(reason ?? String(e), reasonCode ?? failureCode(e));
        }
      } finally {
        checking = false;
      }
    };
    const receive = (e: JobStreamEvent) => {
      if (finished) return;
      last = e.seq;
      switch (e.type) {
        case 'message': case 'progress': case 'state': case 'timeout_warning': {
          try {
            if (JSON.parse(e.data).timeout_warning === true) on.timeoutWarning?.();
          } catch {
          }
          const p = parseProgress(e.data);
          if (p) on.progress(p);
          if (isTerminalState(e.data)) void finish();
          return;
        }
        case 'queue':
          try {
            const position = Number(JSON.parse(e.data).queue_position);
            if (position > 0) on.progress({ stage: 'queued', pct: 0, queuePosition: position });
          } catch {
          }
          return;
        case 'log':
          try {
            const j = JSON.parse(e.data);
            on.log?.(String(j.stream ?? ''), String(j.line ?? ''));
          } catch {
          }
          return;
        case 'result':
          if (!on.result || resulted) return;
          resulted = true;
          void realApi.status(id).then((s) => { if (!finished && !disposed && s.record) on.result!(s); }).catch(() => undefined);
          return;
        case 'done': case 'end': case 'complete': case 'failed': case 'error_event':
          try {
            const data = JSON.parse(e.data);
            reason = data.error || reason;
            reasonCode = data.error_code || reasonCode;
          } catch {
          }
          void finish();
      }
    };
    const follow = () => { if (!finished && !disposed) stop = followJobStream(id, last, receive, () => void finish(true)); };
    follow();
    return () => {
      disposed = true;
      close();
    };
  },
  async cancel(id) {
    await (await mutation(`/api/jobs/${id}`, { method: 'DELETE' })).text();
  },
  downloadUrl: (id) => `/api/jobs/${id}/download`,
  movesUrl: (id) => `/api/jobs/${id}/moves`,
};


export interface SurfaceArtifactManifest {
  validated: boolean;
  preview_ready?: boolean;
  print_qualified?: boolean;
  backend?: string;
  source_sha?: string;
  binary_sha256: string;
  resources_sha256: string;
  features: { curved_tops?: boolean; nonplanar_wave?: boolean };
  wave_modes?: string[];
}
export let toolArtifactManifest: SurfaceArtifactManifest | undefined;
export function applyHealth(health: { artifact_manifest?: SurfaceArtifactManifest }) {
  toolArtifactManifest = health.artifact_manifest;
}
const validatedSurface = (feature: 'curved_tops' | 'nonplanar_wave') => (toolArtifactManifest?.preview_ready ?? toolArtifactManifest?.validated) === true
  && /^[a-f0-9]{64}$/i.test(toolArtifactManifest?.binary_sha256 ?? '')
  && /^[a-f0-9]{64}$/i.test(toolArtifactManifest?.resources_sha256 ?? '')
  && toolArtifactManifest?.features?.[feature] === true;
export const canCurvedTops = () => validatedSurface('curved_tops');
export const canSurfaceWaves = (mode = 'whole') => validatedSurface('nonplanar_wave') && toolArtifactManifest?.wave_modes?.includes(mode) === true;

export function surfaceConfigBlocker(config: Config): string | undefined {
  if (String(config.nonplanar_mode) === 'nonplanar_top' && !canCurvedTops()) return 'Curved layers aren\'t validated in this engine build.';
  const wave = String(config.nonplanar_wave ?? 'off');
  if (wave !== 'off' && !canSurfaceWaves(wave)) return 'Rippled top surfaces aren\'t validated in this engine build.';
}

function guardedApi(api: Api): Api {
  return { ...api, async submit(request, signal, onUpload) {
    if (request.output !== '3mf' && !request.force_planar) {
      const objects = [...request.plate?.objects ?? [], ...request.project_edits?.filter((e) => e.op === 'add_object') ?? []];
      const configs: Config[] = [request.project.machine ?? {}, request.project.process ?? {}, ...request.project.filaments ?? [], request.project_settings ?? {},
        ...request.plate?.plates?.map((p) => p.settings) ?? [],
        ...objects.flatMap((o) => [o.settings ?? {}, ...o.volumes?.map((v) => v.settings ?? {}) ?? [], ...o.ranges?.map((r) => r.settings) ?? []]),
        ...request.project_edits?.flatMap((e) => 'settings' in e ? [e.settings ?? {}] : []) ?? []];
      for (const config of configs) {
        const reason = surfaceConfigBlocker(config);
        if (reason) throw new Error(reason);
      }
    }
    return api.submit(request, signal, onUpload);
  } };
}

export async function connect(timeoutMs = 1500): Promise<Api> {
  bootstrapAccessToken();
  try {
    const r = await readHealth(timeoutMs);
    const type = r.headers.get('content-type') ?? '';
    if (r.ok && type.includes('json')) {
      applyHealth(await r.json() as { artifact_manifest?: SurfaceArtifactManifest });
      return guardedApi(realApi);
    }
  } catch {
  }
  throw new Error('Oops! We lost connection to the slicing service. Reconnect and give it another try.');
}
