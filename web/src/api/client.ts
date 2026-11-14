import { submissionHeaders } from '../flow/submission';
import { bootstrapAccessToken, mutation, responseError } from './access';
import { attachFiles, postJob } from './blobs';
import { noteCapabilities, readHealth } from './health';
import { followJobStream, type JobStreamEvent } from './job-stream';
import { expandCatalog } from './compact-catalog';
import { catalogWorker, onWorkerFailure, workerId } from './worker';
import type { CatalogIn, CatalogOut } from './catalog.worker';
import type { Config, JobRequest, JobStatus, PresetCatalog, ProgressEvent } from './types';

export interface Api {
  mock: boolean;
  presets(vendor: string, models: string[], update?: (c: PresetCatalog) => void, names?: string[]): Promise<PresetCatalog>;
  submit(req: JobRequest, signal?: AbortSignal, onUpload?: (fraction: number) => void): Promise<string>;
  status(id: string): Promise<JobStatus>;
  events(id: string, on: { progress(e: ProgressEvent): void; done(s: JobStatus): void; error(msg: string): void; log?(stream: string, line: string): void; result?(s: JobStatus): void; timeoutWarning?(): void }): () => void;
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

const realApi: Api = {
  mock: false,
  presets(vendor, models, update, names) {
    const worker = catalogWorker();
    const id = workerId();
    return new Promise((resolve, reject) => {
      let answered = false;
      const cleanup = () => { worker.removeEventListener('message', on); unsubscribe(); };
      const fail = (error: Error) => { cleanup(); if (!answered) reject(error); };
      const on = (e: MessageEvent<CatalogOut>) => {
        const m = e.data;
        if (m.id !== id) return;
        if (m.type === 'catalog') {
          const catalog = expandCatalog(m.catalog);
          if (answered) update?.(catalog);
          else resolve(catalog);
          answered = true;
        } else if (m.type === 'done') cleanup();
        else if (m.type === 'error') fail(new Error(m.message));
      };
      const unsubscribe = onWorkerFailure(fail);
      worker.addEventListener('message', on);
      worker.postMessage({ id, vendor, models, ...(names ? { names } : {}), engine: { options: toolFeatures.dump_options === true, presets: toolFeatures.dump_presets === true } } satisfies CatalogIn);
    });
  },

  async submit(req, signal, onUpload) {
    const fd = new FormData();
    await attachFiles(fd, req.files, signal, onUpload);
    const { printer, print, filaments } = req.library;
    if (!printer) throw new Error('Pick a printer first to get started.');
    if (!print) throw new Error('Pick a print profile first.');
    if (!filaments.length || filaments.some((f) => !f)) throw new Error('Please pick a filament for each slot before continuing.');
    fd.append('library', JSON.stringify({ ...req.library, project: req.overrides }));
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
    let reason: string | undefined, stop = () => {}, retry: ReturnType<typeof setTimeout> | undefined;
    const close = () => { finished = true; stop(); clearTimeout(retry); };
    const finish = async (reconnect = false) => {
      if (finished || checking) return;
      checking = true;
      try {
        const s = await realApi.status(id);
        if (disposed) return;
        if (!['done', 'failed', 'cancelled', 'interrupted', 'stale'].includes(s.state)) {
          retry = setTimeout(reconnect ? follow : () => void finish(), 1000);
          return;
        }
        close();
        if (s.state === 'done') on.done(s);
        else on.error(s.error ?? `job ${s.state}`);
      } catch (e) {
        if (!disposed) {
          close();
          on.error(reason ?? String(e));
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
          if (/"state"\s*:\s*"(done|failed|canceled)"/.test(e.data)) void finish();
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
            reason = JSON.parse(e.data).error || reason;
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
    await mutation(`/api/jobs/${id}`, { method: 'DELETE' });
  },
  downloadUrl: (id) => `/api/jobs/${id}/download`,
  movesUrl: (id) => `/api/jobs/${id}/moves`,
};


export let toolFeatures: Record<string, unknown> = {};
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
export function applyHealth(health: { tool_features?: Record<string, unknown>; artifact_manifest?: SurfaceArtifactManifest }) {
  if (!health.tool_features) return;
  toolFeatures = health.tool_features;
  noteCapabilities(health.tool_features);
  toolArtifactManifest = health.artifact_manifest;
}
const validatedSurface = (feature: 'curved_tops' | 'nonplanar_wave') => toolFeatures[feature] === true
  && (toolArtifactManifest?.preview_ready ?? toolArtifactManifest?.validated) === true
  && /^[a-f0-9]{64}$/i.test(toolArtifactManifest?.binary_sha256 ?? '')
  && /^[a-f0-9]{64}$/i.test(toolArtifactManifest?.resources_sha256 ?? '')
  && toolArtifactManifest?.features?.[feature] === true;
export const canCurvedTops = () => validatedSurface('curved_tops');
export const canSurfaceWaves = (mode = 'whole') => validatedSurface('nonplanar_wave') && toolArtifactManifest?.wave_modes?.includes(mode) === true;

export function surfaceConfigBlocker(config: Config): string | undefined {
  if (String(config.nonplanar_mode) === 'nonplanar_top' && !canCurvedTops()) return 'Curved layers require a newer version of the slicing engine.';
  const wave = String(config.nonplanar_wave ?? 'off');
  if (wave !== 'off' && !canSurfaceWaves(wave)) return 'Rippled top surfaces require a newer version of the slicing engine.';
}

function guardedApi(api: Api): Api {
  return { ...api, async submit(request, signal, onUpload) {
    if (request.output !== '3mf' && !request.force_planar) {
      const objects = [...request.plate?.objects ?? [], ...request.project_edits?.filter((e) => e.op === 'add_object') ?? []];
      const configs: Config[] = [request.overrides, request.project_settings ?? {},
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
export const canLoadStep = () => toolFeatures.step === true;
export const canEditProject = () => toolFeatures.edit === true;
export const canForcePlanar = () => toolFeatures.force_planar === true;
export const canCalibrate = () => toolFeatures.calib === true;

export async function connect(timeoutMs = 1500): Promise<Api> {
  bootstrapAccessToken();
  try {
    const r = await readHealth(timeoutMs);
    const type = r.headers.get('content-type') ?? '';
    if (r.ok && type.includes('json')) {
      applyHealth(await r.json() as { tool_features?: Record<string, unknown>; artifact_manifest?: SurfaceArtifactManifest });
      return guardedApi(realApi);
    }
  } catch {
  }
  throw new Error('Oops! We lost connection to the slicing service. Reconnect and give it another try.');
}
