import { execFileSync, spawn } from 'node:child_process';
import { createHash } from 'node:crypto';
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { createServer } from 'node:net';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { gunzipSync, gzipSync } from 'node:zlib';

const here = (path: string) => fileURLToPath(new URL(path, import.meta.url));
const OUT = here('./src/demo/fixtures/sample/');
const SAMPLE = '3DBenchy';
const SLICE_TIMEOUT_MS = 15 * 60_000;

function engine() {
  const dir = process.env.ORCA_ENGINE_DIR ?? '';
  const e = { tool: join(dir, 'nonplanar_slice_tool'), resources: join(dir, 'resources'),
    manifest: join(dir, 'surface-manifest.json'), qualification: join(dir, 'artifact-qualification.ready') };
  if (!dir || ![e.tool, join(e.resources, 'catalog'), e.manifest, e.qualification].every((p) => existsSync(p)))
    throw new Error('The demo build slices its sample project with the engine. Set ORCA_ENGINE_DIR to a released engine '
      + 'directory: nonplanar_slice_tool, resources/ (with catalog/), surface-manifest.json and artifact-qualification.ready.');
  return e;
}

const freePort = () => new Promise<number>((resolve, reject) => {
  const server = createServer().listen(0, '127.0.0.1', () => {
    const { port } = server.address() as { port: number };
    server.close(() => resolve(port));
  }).on('error', reject);
});

const DEMO_FILAMENTS = ['Bambu PLA Basic', 'Bambu PETG HF', 'Bambu PETG Basic', 'Generic PLA', 'Generic PETG'];
export function catalogSlice() {
  const { resources } = engine(), seed = JSON.parse(gunzipSync(readFileSync(here('./src/demo/fixtures/seed.json.gz'))).toString());
  const library = seed.library as { printers: { model: string }[]; filaments: { product: string }[]; processes: { product?: string }[] };
  type Line = { id: string; variants?: { id: string; defaultPrint?: string }[]; tunings?: { fits: string[] }[] };
  const lines = (kind: string) => readFileSync(join(resources, 'catalog', `${kind}.jsonl`), 'utf8').split('\n').filter(Boolean).map((l) => JSON.parse(l));
  const [head, ...printerLines] = lines('printers') as [{ version: string }, ...Line[]];
  const printers = printerLines.filter((p) => library.printers.some((r) => r.model === p.id));
  const variants = new Set(printers.flatMap((p) => p.variants!.map((v) => v.id)));
  const defaults = new Set(printers.flatMap((p) => p.variants!.map((v) => v.defaultPrint).filter(Boolean)));
  const trim = (p: Line) => ({ ...p, tunings: p.tunings!.map((t) => ({ ...t, fits: t.fits.filter((f) => variants.has(f)) })).filter((t) => t.fits.length) });
  const filaments = (lines('filaments').slice(1) as Line[]).filter((p) => DEMO_FILAMENTS.includes(p.id) || library.filaments.some((r) => r.product === p.id)).map(trim);
  const prints = (lines('prints').slice(1) as Line[]).filter((p) => defaults.has(p.id) || library.processes.some((r) => r.product === p.id)).map(trim);
  const out = here('./src/demo/fixtures/catalog.json.gz'), body = gzipSync(JSON.stringify({ version: head.version, printers, filaments, prints }), { level: 9 });
  if (!existsSync(out) || !readFileSync(out).equals(body)) writeFileSync(out, body);
}

export async function sampleSlice(benchy: string) {
  const e = engine();
  const seedBytes = readFileSync(here('./src/demo/fixtures/seed.json.gz')), model = gunzipSync(readFileSync(benchy));
  const stamp = (path?: string) => path ? `${path}:${statSync(path).size}:${statSync(path).mtimeMs}` : '';
  const key = createHash('sha256').update([stamp(e.tool), e.resources, stamp(join(e.resources, 'catalog', 'printers.jsonl')), stamp(e.manifest),
    stamp(e.qualification), readFileSync(here('./demo-sample.ts'), 'utf8')].join('\n')).update(seedBytes).update(model).digest('hex');
  if (existsSync(join(OUT, 'key')) && readFileSync(join(OUT, 'key'), 'utf8') === key) return;

  const seed = JSON.parse(gunzipSync(seedBytes).toString());
  const project = seed.projects.find((p: { meta: { name: string } }) => p.meta.name === SAMPLE);
  if (!project) throw new Error(`The demo seed has no ${SAMPLE} project to slice.`);
  const work = mkdtempSync(join(tmpdir(), 'demo-sample-'));
  mkdirSync(join(work, 'data')); mkdirSync(join(work, 'jobs')); mkdirSync(join(work, 'static'));
  writeFileSync(join(work, 'data', 'library.json'), JSON.stringify({ document: seed.library, operations: {} }));
  execFileSync('go', ['build', '-o', join(work, 'service'), '.'], { cwd: here('../server'), stdio: 'inherit' });
  const port = await freePort(), base = `http://127.0.0.1:${port}`;
  const service = spawn(join(work, 'service'), ['-listen', `127.0.0.1:${port}`, '-tool', e.tool, '-resources', e.resources,
    '-surface-manifest', e.manifest, '-artifact-qualification', e.qualification, '-data-dir', join(work, 'data'),
    '-job-root', join(work, 'jobs'), '-static', join(work, 'static'), '-max-jobs', '1', '-job-ttl', '0', '-printer-network', 'off'],
  { stdio: ['ignore', 'ignore', 'pipe'] });
  let log = '';
  service.stderr.on('data', (b: Buffer) => { log = (log + b.toString()).slice(-4000); });
  const call = async (path: string, init?: RequestInit) => {
    const r = await fetch(base + path, init);
    if (!r.ok) throw new Error(`${init?.method ?? 'GET'} ${path}: ${r.status} ${await r.text()}`);
    return r;
  };
  try {
    for (let i = 0; ; i++) {
      if (await fetch(`${base}/api/healthz`).then((r) => r.ok, () => false)) break;
      if (i > 120) throw new Error(`The service did not start:\n${log}`);
      await new Promise((r) => setTimeout(r, 250));
    }
    const { printer, process: print, filamentSlots: filaments } = project.doc;
    const slots = (seed.library.printers.find((p: { id: string }) => p.id === printer)?.slots ?? []).slice(0, filaments.length).map((slot: { id: string }) => slot.id);
    const answer = await (await call('/api/resolve', { method: 'POST', headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ printer, print, filaments, slots, layerHeight: null, project: { machine: {}, process: {}, filaments: filaments.map(() => ({})) } }) })).json();
    const upload = Buffer.from(model), faces = upload.readUInt32LE(80), at = (f: number, v: number) => 84 + f * 50 + 12 + v * 12 + 8;
    let bottom = Infinity;
    for (let f = 0; f < faces; f++) for (let v = 0; v < 3; v++) bottom = Math.min(bottom, upload.readFloatLE(at(f, v)));
    for (let f = 0; f < faces; f++) for (let v = 0; v < 3; v++) upload.writeFloatLE(upload.readFloatLE(at(f, v)) - bottom, at(f, v));
    const sha = createHash('sha256').update(upload).digest('hex');
    await call(`/api/blobs/${sha}`, { method: 'PUT', body: upload });
    const object = project.doc.objects[0], file = `${SAMPLE}.stl`;
    const form = new FormData();
    form.set('files', JSON.stringify([{ name: file, sha256: sha }]));
    form.set('library', JSON.stringify({ printer, print, filaments, slots, layerHeight: answer.print.heights?.chosen ?? null,
      project: { machine: {}, process: {}, filaments: filaments.map(() => ({})) } }));
    form.set('plate', JSON.stringify({ objects: [{ file, name: SAMPLE, extruder: object.extruder,
      instances: object.instances.map((i: { x: number; y: number; rotZ: number }) => ({ x: i.x, y: i.y, rot: i.rotZ, z: 0, printable: true })) }] }));
    form.set('output', 'gcode');
    const { id } = await (await call('/api/jobs', { method: 'POST', body: form })).json();
    const started = Date.now();
    let status: { state: string; error?: string; record?: { inputs?: { mode?: string } } };
    for (;;) {
      status = await (await call(`/api/jobs/${id}`)).json();
      if (status.state === 'done') break;
      if (status.state === 'failed' || status.state === 'canceled') throw new Error(`The sample slice failed: ${status.error ?? status.state}`);
      if (Date.now() - started > SLICE_TIMEOUT_MS) throw new Error('The sample slice did not finish in 15 minutes.');
      await new Promise((r) => setTimeout(r, 1000));
    }
    if (status.record?.inputs?.mode !== 'nonplanar_top')
      throw new Error(`The sample sliced as "${status.record?.inputs?.mode}", not Curved tops: the service holds the engine in ORCA_ENGINE_DIR unqualified (see its /api/healthz reason).`);
    const bytes = async (path: string) => Buffer.from(await (await call(path)).arrayBuffer());
    rmSync(OUT, { recursive: true, force: true }); mkdirSync(OUT, { recursive: true });
    writeFileSync(join(OUT, 'status.json.gz'), gzipSync(JSON.stringify(status), { level: 9 }));
    writeFileSync(join(OUT, 'moves.gz'), gzipSync(await bytes(`/api/jobs/${id}/moves`), { level: 9 }));
    writeFileSync(join(OUT, 'gcode.gz'), gzipSync(await bytes(`/api/jobs/${id}/download`), { level: 9 }));
    writeFileSync(join(OUT, 'surface.json.gz'), gzipSync(await bytes(`/api/jobs/${id}/surface-grid`), { level: 9 }));
    const engineReads = { health: await (await call('/api/healthz')).json(), options: await (await call('/api/options')).json(),
      tooltips: await (await call('/api/options/tooltips')).json() };
    writeFileSync(join(OUT, 'engine.json.gz'), gzipSync(JSON.stringify(engineReads), { level: 9 }));
    writeFileSync(join(OUT, 'project.json.gz'), gzipSync(JSON.stringify({ project: project.meta.id })));
    writeFileSync(join(OUT, 'key'), key);
  } finally {
    service.kill();
    rmSync(work, { recursive: true, force: true });
  }
}
