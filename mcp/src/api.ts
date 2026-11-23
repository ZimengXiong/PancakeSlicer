import { createHash } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import { basename } from 'node:path';

export class ApiError extends Error {
  constructor(message: string, readonly status: number, readonly code?: string, readonly body?: unknown) {
    super(message);
  }
}

export interface ProfileRef { id: string; name: string }
export interface LibrarySlot { id: string; tray?: string; filament: string }
export interface LibraryPrinter extends ProfileRef {
  model: string;
  vendor?: string;
  process?: string;
  plate?: string;
  nozzles?: { diameter: number; variant: string }[];
  slots: LibrarySlot[];
  swap?: string;
}
export interface LibraryFilament extends ProfileRef { product?: string; settings?: Record<string, unknown> }
export interface LibraryDocument {
  printers: LibraryPrinter[];
  processes: ProfileRef[];
  filaments: LibraryFilament[];
}

export interface JobStatus {
  id: string;
  state: string;
  stage?: string;
  progress?: number;
  error?: string;
  error_code?: string;
  filename?: string;
  queue_position?: number;
  eta_seconds?: number;
  record?: Record<string, any>;
}

export interface Connection { printer: string; id: string; name: string; kind: string; host?: string; model?: string }
export interface LiveSnapshot {
  enabled: boolean;
  connections: Connection[];
  status?: Record<string, any>[];
  transfers?: Record<string, any>[];
}

export class PancakeApi {
  constructor(readonly base: string, readonly token?: string) {}

  private headers(extra: Record<string, string> = {}) {
    return this.token ? { Authorization: `Bearer ${this.token}`, ...extra } : extra;
  }

  async request(path: string, init: RequestInit = {}): Promise<Response> {
    let response: Response;
    try {
      response = await fetch(this.base + path, { ...init, headers: this.headers(init.headers as Record<string, string>) });
    } catch (error) {
      throw new ApiError(`PancakeSlicer isn't answering at ${this.base} (${(error as Error).message}). Start the app or set PANCAKESLICER_URL.`, 0);
    }
    if (!response.ok) {
      const text = await response.text();
      let body: any;
      try { body = JSON.parse(text); } catch { body = text; }
      const message = typeof body === 'object' && body?.error ? body.error : text || response.statusText;
      throw new ApiError(`${init.method ?? 'GET'} ${path}: ${response.status} ${message}`, response.status, body?.code, body);
    }
    return response;
  }

  async json<T>(path: string, init: RequestInit = {}): Promise<T> {
    return (await this.request(path, init)).json() as Promise<T>;
  }

  health() { return this.json<Record<string, any>>('/api/healthz'); }

  async library(): Promise<LibraryDocument> {
    const answer = await this.json<{ document: LibraryDocument }>('/api/library');
    return answer.document;
  }

  async uploadBlob(data: Uint8Array<ArrayBuffer>): Promise<string> {
    const sum = createHash('sha256').update(data).digest('hex');
    const { missing } = await this.json<{ missing: string[] }>('/api/blobs/missing', {
      method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ sha256: [sum] }),
    });
    if (missing.includes(sum)) await this.request(`/api/blobs/${sum}`, { method: 'PUT', body: data });
    return sum;
  }

  async submitJob(path: string, form: Record<string, string>): Promise<string> {
    const data = new Uint8Array(await readFile(path)) as Uint8Array<ArrayBuffer>;
    const name = basename(path);
    const sum = await this.uploadBlob(data);
    const post = async () => {
      const fd = new FormData();
      fd.set('files', JSON.stringify([{ name, sha256: sum }]));
      for (const [key, value] of Object.entries(form)) fd.set(key, value);
      return this.json<{ id: string }>('/api/jobs', { method: 'POST', body: fd });
    };
    try {
      return (await post()).id;
    } catch (error) {
      if ((error as ApiError).code !== 'blob_missing') throw error;
      await this.request(`/api/blobs/${sum}`, { method: 'PUT', body: data });
      return (await post()).id;
    }
  }

  job(id: string) { return this.json<JobStatus>(`/api/jobs/${encodeURIComponent(id)}`); }

  async waitJob(id: string, timeoutS: number): Promise<JobStatus> {
    const until = Date.now() + timeoutS * 1000;
    for (;;) {
      const status = await this.job(id);
      if (status.state !== 'queued' && status.state !== 'running') return status;
      if (Date.now() > until) return status;
      await new Promise((resolve) => setTimeout(resolve, 1000));
    }
  }

  cancelJob(id: string) { return this.request(`/api/jobs/${encodeURIComponent(id)}/cancel`, { method: 'POST' }); }

  async download(id: string): Promise<{ data: Uint8Array; filename: string }> {
    const response = await this.request(`/api/jobs/${encodeURIComponent(id)}/download`);
    const disposition = response.headers.get('content-disposition') ?? '';
    const match = /filename\*=UTF-8''([^;]+)|filename="?([^";]+)"?/i.exec(disposition);
    const filename = match ? decodeURIComponent(match[1] ?? match[2]) : `${id}.gcode.3mf`;
    return { data: new Uint8Array(await response.arrayBuffer()), filename };
  }

  async live(timeoutMs = 8000): Promise<LiveSnapshot> {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), timeoutMs);
    try {
      const response = await this.request('/api/events', { signal: controller.signal });
      const reader = response.body!.getReader();
      const decoder = new TextDecoder();
      let buffer = '';
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        buffer += decoder.decode(value, { stream: true });
        for (let end = buffer.indexOf('\n\n'); end >= 0; end = buffer.indexOf('\n\n')) {
          const block = buffer.slice(0, end);
          buffer = buffer.slice(end + 2);
          const lines = block.split('\n');
          if (lines.includes('event: connections')) {
            const data = lines.filter((l) => l.startsWith('data: ')).map((l) => l.slice(6)).join('\n');
            controller.abort();
            return JSON.parse(data) as LiveSnapshot;
          }
        }
      }
    } catch (error) {
      if (!controller.signal.aborted) throw error;
    } finally {
      clearTimeout(timer);
    }
    const listed = await this.json<{ connections: Connection[]; enabled: boolean }>('/api/printer-connections');
    return { enabled: listed.enabled, connections: listed.connections };
  }

  sendPlan(job: string, printer: string) {
    return this.json<{ slots: Record<string, any>[] }>(`/api/jobs/${encodeURIComponent(job)}/send/plan?printer=${encodeURIComponent(printer)}`);
  }

  send(job: string, body: Record<string, unknown>) {
    return this.json<Record<string, any>>(`/api/jobs/${encodeURIComponent(job)}/send`, {
      method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body),
    });
  }

  sendState(job: string) { return this.json<Record<string, any>>(`/api/jobs/${encodeURIComponent(job)}/send`); }

  device(printer: string, body: Record<string, unknown>) {
    return this.json<Record<string, any>>(`/api/printer-connections/${encodeURIComponent(printer)}/device`, {
      method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body),
    });
  }

  control(printer: string, action: 'pause' | 'resume' | 'stop') {
    return this.json<Record<string, any>>(`/api/printer-connections/${encodeURIComponent(printer)}/control`, {
      method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ action }),
    });
  }

  cameras(printer: string) {
    return this.json<{ cameras: { id: string; name: string; format: string; url: string }[] }>(`/api/printer-connections/${encodeURIComponent(printer)}/camera`);
  }

  async cameraFrame(printer: string, camera: string, timeoutMs = 20000): Promise<Uint8Array> {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), timeoutMs);
    try {
      const response = await this.request(`/api/printer-connections/${encodeURIComponent(printer)}/camera/${encodeURIComponent(camera)}`, { signal: controller.signal });
      const reader = response.body!.getReader();
      let buffer = new Uint8Array(0);
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        const next = new Uint8Array(buffer.length + value.length);
        next.set(buffer);
        next.set(value, buffer.length);
        buffer = next;
        const start = indexOf(buffer, [0xff, 0xd8, 0xff], 0);
        if (start < 0) continue;
        const end = indexOf(buffer, [0xff, 0xd9], start + 3);
        if (end >= 0) {
          controller.abort();
          return buffer.slice(start, end + 2);
        }
      }
    } finally {
      clearTimeout(timer);
    }
    throw new ApiError('The camera stream ended before a whole frame arrived.', 0);
  }
}

function indexOf(haystack: Uint8Array, needle: number[], from: number) {
  outer: for (let i = from; i <= haystack.length - needle.length; i++) {
    for (let j = 0; j < needle.length; j++) if (haystack[i + j] !== needle[j]) continue outer;
    return i;
  }
  return -1;
}
