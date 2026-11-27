import { LIBRARY_SCHEMA, type LibraryRecords } from '../profiles/records';
import type { DemoCatalog } from './resolver';
import { NO_ENGINE, SAMPLE_JOB } from './config';
import { GITHUB_URL } from '../build-info';
type Row = Record<string, any>;
interface Seed { library: LibraryRecords; preferences: Row; session: Row; printCatalog: Row[]; projects: Row[] }
interface DemoState { library: LibraryRecords; preferences: Row; session: Row; projects: Record<string, Row>; drafts: Record<string, Row>;
  assets: Record<string, string>; versions: Record<string, Row[]>; folders: Row[]; folderRevision: number }
type Resolve = (library: LibraryRecords, body: unknown, options: unknown, catalog: DemoCatalog) => Promise<unknown>;
let hooks: { notice?(code: 'demo_storage_full'): void; resolve?: Resolve; slotOffer?: Resolve } = {};
export const configureDemo = (value: typeof hooks) => { hooks = value; };
const STORAGE = 'pancake.demo.v1';
const files = import.meta.glob(['./fixtures/*', './fixtures/assets/*', './fixtures/sample/*'], { eager: true, query: '?url&no-inline', import: 'default' }) as Record<string, string>;
const nativeFetch = globalThis.fetch.bind(globalThis), mainThread = typeof window !== 'undefined';
let installed = false, seedRead: Promise<Seed> | undefined, stateRead: Promise<DemoState> | undefined;
const json = (value: unknown, status = 200) => new Response(JSON.stringify(value), { status, headers: { 'Content-Type': 'application/json' } });
const missing = () => json({ error: 'This item isn\'t available in the live demo.' }, 404);
const assetUrl = (name: string) => { const url = files[`./fixtures/${name}`]; if (!url) throw new Error(`Oops! Demo sample not found: ${name}`); return url; };
async function compressed(name: string) {
  const response = await nativeFetch(assetUrl(name));
  if (!response.ok || !response.body) throw new Error(`Oops! Couldn't load demo sample: ${name}`);
  return new Response(response.body.pipeThrough(new DecompressionStream('gzip')));
}
const sampleAsset = (key: string) => files[`./fixtures/assets/${key}.gz`] ? compressed(`assets/${key}.gz`) : null;
const seed = () => seedRead ??= compressed('seed.json.gz').then(r => r.json() as Promise<Seed>);
const later = <T,>(name: string) => { let read: Promise<T> | undefined; return () => read ??= compressed(name).then(r => r.json() as Promise<T>); };
const covers = later<Record<string, string>>('covers.json.gz'), engine = later<{ health: Row; options: Row; tooltips: Row }>('sample/engine.json.gz');
const sampleStatus = later<Row>('sample/status.json.gz');
const sampleProject = later<{ project: string }>('sample/project.json.gz');
let sampleLines: Promise<string[]> | undefined;
async function sampleJob(action: string | undefined, url: URL, signal: AbortSignal): Promise<Response> {
  const status = { ...await sampleStatus(), id: SAMPLE_JOB, download_url: `/api/jobs/${SAMPLE_JOB}/download`, events_url: `/api/jobs/${SAMPLE_JOB}/events` };
  if (!action) return json(status);
  if (action === 'events') {
    const stream = new ReadableStream<Uint8Array>({ start(controller) {
      if (!signal.aborted) controller.enqueue(new TextEncoder().encode(`id: 0\nevent: done\ndata: ${JSON.stringify(status)}\n\n`));
      controller.close();
    } });
    return new Response(stream, { headers: { 'Content-Type': 'text/event-stream' } });
  }
  if (action === 'moves') return compressed('sample/moves.gz');
  if (action === 'download') return compressed('sample/gcode.gz');
  if (action === 'surface-grid') return compressed('sample/surface.json.gz');
  if (action === 'transitions') return json([]);
  if (action === 'gcode-lines') {
    const lines = await (sampleLines ??= compressed('sample/gcode.gz').then(r => r.text()).then(t => t.split('\n')));
    const at = Number(url.searchParams.get('block') ?? 0) * 1024;
    return json({ total: lines.length, lines: lines.slice(at, at + 1024) });
  }
  return json({ error: NO_ENGINE }, 503);
}
let catalogRead: Promise<DemoCatalog> | undefined;
const catalog = () => catalogRead ??= compressed('catalog.json.gz').then(r => r.json() as Promise<DemoCatalog>);
function initial(s: Seed): DemoState { return structuredClone({ library: { ...s.library, schema: LIBRARY_SCHEMA }, preferences: s.preferences, session: s.session,
  projects: Object.fromEntries(s.projects.map(p => [p.meta.id, p])), drafts: {}, assets: {}, versions: {}, folders: [], folderRevision: 0 }); }
async function state(): Promise<DemoState> {
  return stateRead ??= seed().then(s => {
    if (mainThread) { try {
      const saved = sessionStorage.getItem(STORAGE);
      if (saved) {
        const data = JSON.parse(saved) as DemoState;
        return Number(data.library.schema) === 13 ? { ...data, library: { ...data.library, schema: LIBRARY_SCHEMA } } : data;
      }
    } catch {  } }
    return initial(s);
  });
}
function save(s: DemoState) {
  if (!mainThread) return;
  try { sessionStorage.setItem(STORAGE, JSON.stringify(s)); }
  catch { hooks.notice?.('demo_storage_full'); }
}
export async function resetDemo() {
  const next = initial(await seed());
  next.preferences = { revision: 0, document: {} };
  next.session = { revision: 0, document: { demoOnboarding: true } };
  stateRead = Promise.resolve(next); save(next);
}
// Firefox has no Request.body, so read the payload through blob() in every browser.
async function body(request: Request): Promise<Row> {
  const blob = await request.blob();
  const text = request.headers.get('content-encoding') === 'gzip'
    ? await new Response(blob.stream().pipeThrough(new DecompressionStream('gzip'))).text() : await blob.text();
  return text ? JSON.parse(text) : {};
}
function version(s: DemoState, id: string) {
  const p = s.projects[id]; if (!p) return null;
  const list = s.versions[id] ??= []; let v = list.at(-1);
  if (!v || v.revision !== p.meta.revision) {
    v = { id: list.length + 1, revision: p.meta.revision, at: p.meta.savedAt, until: p.meta.savedAt, name: p.meta.name,
      objects: p.meta.objects, record: String(p.meta.revision), change: [{ label: list.length ? 'Edited' : 'Created' }], project: structuredClone(p) }; list.push(v);
  }
  return v;
}
async function base64(blob: Blob) {
  const bytes = new Uint8Array(await blob.arrayBuffer()), pieces: string[] = [];
  for (let at = 0; at < bytes.length; at += 16384) pieces.push(String.fromCharCode(...bytes.subarray(at, at + 16384)));
  return btoa(pieces.join(''));
}
function printerEntry(p: Row, captured: Record<string, string>) {
  const defaults = (p.variants as Row[]).filter(v => v.default), first = defaults.find(v => v.nozzle === '0.4') ?? defaults[0];
  const shot = captured[`printer-catalog/covers/${p.vendor}/${p.id}`], values = { ...p.base, ...first?.values };
  const area = String(values.printable_area ?? '').split(',').map(pt => pt.split('x').map(Number));
  const span = (k: 0 | 1) => Math.max(...area.map(a => a[k])) - Math.min(...area.map(a => a[k]));
  return { id: p.id, vendor: p.vendor, vendorName: p.vendor_name || p.vendor, family: p.model?.family, bed: area.length > 2 ? `${span(0)}x${span(1)}x${values.printable_height}` : undefined,
    cover: shot ? `data:image/png;base64,${shot}` : undefined, extruders: String(values.nozzle_diameter ?? '0.4').split(',').length,
    nozzles: defaults.map(v => String(v.nozzle)).sort((a, b) => Number(a) - Number(b)) };
}
const from64 = (value: string) => new Response(Uint8Array.from(atob(value), c => c.charCodeAt(0)));
async function handle(request: Request): Promise<Response> {
  const url = new URL(request.url), path = decodeURIComponent(url.pathname).slice(5), method = request.method, data = await seed();
  if (path === 'healthz') return json((await engine()).health);
  if (path === 'update') return json({ release: null });
  if (path === 'options') return json({ ...(await engine()).options, devReset: false });
  if (path === 'options/tooltips') return json((await engine()).tooltips);
  if (path === 'print-catalog') return json(data.printCatalog);
  if (path.startsWith('catalog/')) {
    const [, kind, id] = path.split('/') as [string, keyof Omit<DemoCatalog, 'version'>, string | undefined], c = await catalog(), list = c[kind];
    if (!Array.isArray(list)) return missing();
    if (id === undefined) {
      const shots = kind === 'printers' ? await covers() : {};
      return json({ version: c.version, [kind]: list.map(p => kind === 'printers' ? printerEntry(p, shots) : { id: p.id, brand: p.brand, type: p.type }) });
    }
    const line = list.find(p => p.id === id); return line ? json(line) : missing();
  }
  if (path === 'user-heads' || path === 'printer-sends') return json([]);
  if (path === 'emboss-fonts') return json({ fonts: [] });
  if (path === 'printer-connections') return json({ enabled: true, connections: [] });
  if (path.startsWith('printer-connections/')) return json({ error: 'Not available in demo mode' }, 400);
  if (path.startsWith('project-leases/')) return json({ granted: true, heartbeatMs: 60000 });
  if (path.startsWith('handy-models/')) {
    const model = path.slice('handy-models/'.length).toLowerCase();
    const sample = data.projects.find(p => /benchy/.test(model) ? /benchy/i.test(p.meta.name) : /cube/i.test(p.meta.name));
    const key = sample?.doc.objects[0]?.source;
    return (key && sampleAsset(key)) || missing();
  }
  if (path === 'about') return json({ name: 'PancakeSlicer Demo', version: 'Client-side demo', source_url: GITHUB_URL, source_configured: true, license: 'AGPL-3.0', revision: (await engine()).health.server_revision });
  const s = await state();
  if (path === 'resolve' || path === 'slot-offer') {
    const run = path === 'resolve' ? hooks.resolve : hooks.slotOffer;
    if (!run) return json({ error: 'The demo resolver only runs right here on the page.' }, 500);
    try { return json(await run(s.library, await body(request), (await engine()).options.options, await catalog())); }
    catch (error) { return json({ error: error instanceof Error ? error.message : String(error) }, 500); }
  }
  if (path === 'library') {
    if (method === 'GET') return json({ document: s.library, problems: {}, notices: [] });
    const input = await body(request); s.library = { ...input.document, revision: input.expectedRevision + 1 }; save(s);
    return json({ revision: s.library.revision, operationId: input.operationId, document: s.library, problems: {}, notices: [] });
  }
  if (path === 'session' || path === 'preferences') {
    if (method === 'GET') return json(s[path]);
    const input = await body(request); s[path] = { revision: input.expectedRevision + 1, document: input.document }; save(s);
    return json({ revision: s[path].revision, operationId: input.operationId });
  }
  if (path.startsWith('assets/') || path.startsWith('blobs/')) {
    const key = path.slice(path.indexOf('/') + 1); if (key === 'missing') return json({ missing: [] });
    if (method === 'PUT') { let blob = await request.blob(); if (request.headers.get('content-encoding') === 'gzip') blob = await new Response(blob.stream().pipeThrough(new DecompressionStream('gzip'))).blob(); s.assets[key] = await base64(blob); save(s); return json({}); }
    if (s.assets[key]) return method === 'HEAD' ? new Response(null) : from64(s.assets[key]);
    if (!files[`./fixtures/assets/${key}.gz`]) return missing();
    return method === 'HEAD' ? new Response(null) : (sampleAsset(key) ?? missing());
  }
  if (path === 'drafts') return json(Object.values(s.drafts));
  if (path.startsWith('drafts/')) {
    const id = path.slice(7);
    if (method === 'DELETE') { delete s.drafts[id]; save(s); return json({}); }
    if (method === 'PUT') { const input = await body(request); s.drafts[id] = { ...input, id }; save(s); return json({ sequence: input.sequence }); }
    return s.drafts[id] ? json(s.drafts[id]) : missing();
  }
  if (path === 'projects/references') return json({ complete: true, documents: Object.values(s.projects).map(p => ({ id: p.meta.id, name: p.meta.name, doc: p.doc, trashed: !!p.archived })) });
  if (path === 'projects') return json(Object.values(s.projects).filter(p => !!p.archived === (url.searchParams.get('archived') === '1')).map(p => p.meta).sort((a, b) => b.savedAt - a.savedAt));
  if (path.startsWith('projects/')) {
    const [, id, action] = path.split('/'), p = s.projects[id];
    if (method === 'GET') return p ? json(p) : missing();
    const input = await body(request);
    if (method === 'PUT') { const revision = input.expectedRevision + 1; s.projects[id] = { ...input.record, meta: { ...input.record.meta, id, revision } }; version(s, id); save(s); return json({ revision, operationId: input.operationId }); }
    if (!p) return missing();
    if (action === 'duplicate') { const copy = structuredClone(p); copy.meta = { ...copy.meta, id: input.newId, name: input.name, revision: 1, savedAt: Date.now() }; s.projects[input.newId] = copy; version(s, input.newId); save(s); return json(copy.meta); }
    if (action === 'archive' || action === 'restore') p.archived = action === 'archive';
    if (action === 'rename') p.meta.name = input.name;
    p.meta.revision = input.expectedRevision + 1; version(s, id); save(s); return json({ revision: p.meta.revision, operationId: input.operationId });
  }
  if (path.startsWith('versions/')) {
    const [, id, v, action] = path.split('/'), latest = version(s, id), list = s.versions[id] ?? [];
    if (v === 'seal') { if (latest) latest.sealed = true; save(s); return json(latest); }
    if (!v) return json({ revision: s.projects[id]?.meta.revision ?? 0, versions: [...list].reverse().map(({ project: _, ...row }) => row) });
    const found = list.find(row => row.id === Number(v)); if (!found) return missing();
    if (action === 'summary') return json(found.project);
    return json(found.project);
  }
  if (path === 'folders') {
    if (method === 'GET') return json({ revision: s.folderRevision, folders: s.folders });
    const input = await body(request), folder = { id: input.id, name: input.name, parentId: input.parentId ?? null, revision: 1, createdAt: Date.now(), updatedAt: Date.now() }; s.folders.push(folder); s.folderRevision++; save(s); return json({ revision: s.folderRevision, folder });
  }
  if (path.startsWith('folders/')) {
    const [, id, action] = path.split('/'), input = await body(request);
    if (id === 'move') { for (const key of input.ids ?? input.projectIds ?? []) if (s.projects[key]) s.projects[key].meta.folderId = input.folderId; }
    else if (action === 'delete') {
      const parent = s.folders.find(f => f.id === id)?.parentId ?? null;
      s.folders = s.folders.filter(f => f.id !== id).map(f => f.parentId === id ? { ...f, parentId: parent } : f);
      for (const p of Object.values(s.projects)) if (p.meta.folderId === id) p.meta.folderId = null;
    }
    else { const folder = s.folders.find(f => f.id === id); if (folder) Object.assign(folder, input); }
    s.folderRevision++; save(s); return json({ revision: s.folderRevision });
  }
  if (path === 'demo/sample') return json({ ...await sampleProject(), job: SAMPLE_JOB });
  if (path.startsWith(`jobs/${SAMPLE_JOB}`)) return sampleJob(path.split('/')[2], url, request.signal);
  if (path === 'jobs' || path === 'patch-preview' || path.startsWith('jobs/')) return json({ error: NO_ENGINE }, 503);
  return missing();
}
export function installDemoTransport() {
  if (installed) return; installed = true;
  globalThis.fetch = async (input, init) => {
    const raw = input instanceof Request ? input.url : String(input), url = new URL(raw, globalThis.location.href);
    if (!url.pathname.startsWith('/api/')) return nativeFetch(input, init);
    const request = new Request(url, input instanceof Request ? input : init);
    if (request.signal.aborted) throw new DOMException('Request aborted', 'AbortError');
    return handle(request);
  };
  if (mainThread) {
    const beacon = navigator.sendBeacon.bind(navigator);
    navigator.sendBeacon = (url, data) => String(url).startsWith('/api/') || beacon(url, data);
    if (!location.hash) void state().then(s => {
      const id = s.session.document.demoProject ?? Object.keys(s.projects)[0];
      if (id && !s.session.document.demoOnboarding && s.library.printers?.length) history.replaceState(null, '', `${location.pathname}#/p/${id}`);
    });
  }
}
