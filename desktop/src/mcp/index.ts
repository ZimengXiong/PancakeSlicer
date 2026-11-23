import { mkdir, readFile, stat, writeFile } from 'node:fs/promises';
import { basename, dirname, extname, isAbsolute, join, resolve } from 'node:path';
import { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js';
import { StdioServerTransport } from '@modelcontextprotocol/sdk/server/stdio.js';
import { z } from 'zod';
import { ApiError, PancakeApi, type JobStatus, type LibraryDocument, type LibraryPrinter, type ProfileRef } from './api.js';
import { inspectOutput } from './gcode.js';

const PREFERENCE = 'Preferences › AI agents';
const connectionArg = process.argv.indexOf('--connection');
const connectionPath = connectionArg >= 0 ? process.argv[connectionArg + 1] : undefined;

let api: PancakeApi;
let printerAccess = false;

function alive(pid: unknown) {
  if (typeof pid !== 'number') return true;
  try { process.kill(pid, 0); return true; } catch (error) { return (error as NodeJS.ErrnoException).code === 'EPERM'; }
}

async function connect() {
  if (process.env.PANCAKESLICER_URL) {
    api = new PancakeApi(process.env.PANCAKESLICER_URL.replace(/\/+$/, ''), process.env.PANCAKESLICER_TOKEN || undefined);
    printerAccess = process.env.PANCAKESLICER_PRINTER_ACCESS === '1';
    windowControl = null;
    return;
  }
  if (!connectionPath) throw new Error(`This MCP server is set up from PancakeSlicer's ${PREFERENCE}, or set PANCAKESLICER_URL for a self-hosted server.`);
  let connection: { url?: string; token?: string; printers?: boolean; pid?: number; control?: string; controlToken?: string };
  try {
    connection = JSON.parse(await readFile(connectionPath, 'utf8'));
  } catch {
    throw new Error(`PancakeSlicer isn't letting AI agents in. Open PancakeSlicer and turn on "Let AI agents use PancakeSlicer" in ${PREFERENCE}.`);
  }
  if (!connection.url || !alive(connection.pid)) throw new Error("PancakeSlicer isn't running. Open it (AI agent access stays on) and try again.");
  api = new PancakeApi(connection.url.replace(/\/+$/, ''), connection.token || undefined);
  printerAccess = connection.printers === true;
  windowControl = connection.control && connection.controlToken ? { url: connection.control, token: connection.controlToken } : null;
}

let windowControl: { url: string; token: string } | null = null;

interface WindowState {
  tab: string; project: string | null; plate: number; printer: string; process: string;
  filament_slots: { slot: number; filament: string }[];
  objects: { id: number; name: string; slot: number }[];
  job: { id?: string; state: string; stage?: string; progress?: number; stale?: boolean; error?: string; error_code?: string };
}

function needWindow() {
  if (!windowControl) throw new Error('This needs the PancakeSlicer desktop window, which is not connected. Use background: true to slice without it.');
  return windowControl;
}

async function inWindow(command: string, args: Record<string, unknown> = {}): Promise<WindowState> {
  const control = needWindow();
  let response: Response;
  try {
    response = await fetch(`${control.url}/command`, {
      method: 'POST', headers: { Authorization: `Bearer ${control.token}`, 'Content-Type': 'application/json' }, body: JSON.stringify({ command, args }),
    });
  } catch {
    throw new Error("PancakeSlicer's window isn't answering. Is the app open?");
  }
  const body = await response.json().catch(() => ({})) as { result?: WindowState; error?: string };
  if (!response.ok) throw new Error(body.error ?? `PancakeSlicer refused "${command}" (${response.status}).`);
  return body.result as WindowState;
}

function needPrinterAccess() {
  if (!printerAccess) throw new Error(`PancakeSlicer doesn't let AI agents use printers. Ask the user to turn on "Let agents send prints and control printers" in ${PREFERENCE}${process.env.PANCAKESLICER_URL ? ' (or set PANCAKESLICER_PRINTER_ACCESS=1)' : ''}.`);
}

const server = new McpServer({ name: 'pancakeslicer', version: '0.1.0' }, {
  instructions: [
    'Drives the running PancakeSlicer app: profiles, slicing, G-code checks and, when the user allows it, its LAN printers. The user controls access in PancakeSlicer\'s Preferences › AI agents; if a tool says access is off, tell the user where to turn it on instead of retrying.',
    'Work happens in the open PancakeSlicer window so the user can watch: slice_model opens the file as a new project and slices it; adjust_plate and slice_plate change and re-slice what is open; app_state reads it. Then inspect_gcode when the filament or temperatures matter.',
    'Before send_job: printer_status, then printer_light on and camera_snapshot to check that the plate is empty, then send_plan to read the filament to tray mapping.',
    'Never call send_job or printer_control without the user\'s explicit go-ahead for that exact job or action; approval for one job does not cover the next.',
    'A send_plan slot whose expected and reported filament types differ means the job would pull the wrong material. Stop and tell the user rather than sending.',
  ].join(' '),
});

type Content = { type: 'text'; text: string } | { type: 'image'; data: string; mimeType: string };

function reply(value: unknown, extra: Content[] = []) {
  const text = typeof value === 'string' ? value : JSON.stringify(value, null, 2);
  return { content: [{ type: 'text' as const, text }, ...extra] };
}

function failure(error: unknown) {
  const e = error as ApiError;
  const detail = e instanceof ApiError && e.body && typeof e.body === 'object' ? `\n${JSON.stringify(e.body, null, 2)}` : '';
  return { isError: true, content: [{ type: 'text' as const, text: `${e.message ?? String(error)}${detail}` }] };
}

function tool<T extends z.ZodRawShape>(name: string, config: { title: string; description: string; inputSchema: T; annotations?: Record<string, boolean>; printers?: boolean },
  run: (args: z.objectOutputType<T, z.ZodTypeAny>) => Promise<{ content: Content[] }>) {
  const { printers, ...definition } = config;
  server.registerTool(name, definition, (async (args: z.objectOutputType<T, z.ZodTypeAny>) => {
    try {
      await connect();
      if (printers) needPrinterAccess();
      return await run(args);
    } catch (error) {
      return failure(error);
    }
  }) as any);
}

function pick<T extends ProfileRef>(list: T[], wanted: string | undefined, kind: string): T {
  if (!wanted) {
    if (!list.length) throw new Error(`There is no ${kind} in the PancakeSlicer library yet.`);
    return list[0];
  }
  const lower = wanted.toLowerCase();
  const found = list.find((p) => p.id === wanted) ?? list.find((p) => p.name.toLowerCase() === lower)
    ?? (() => {
      const partial = list.filter((p) => p.name.toLowerCase().includes(lower));
      if (partial.length > 1) throw new Error(`"${wanted}" matches several ${kind}s: ${partial.map((p) => p.name).join(', ')}.`);
      return partial[0];
    })();
  if (!found) throw new Error(`No ${kind} called "${wanted}". Known: ${list.map((p) => p.name).join(', ')}.`);
  return found;
}

const nameOf = (lib: LibraryDocument, id: string) => lib.filaments.find((f) => f.id === id)?.name ?? (id ? id : '(empty)');

function slotIndex(printer: LibraryPrinter, slot: number | string | undefined) {
  if (slot === undefined || slot === '') return 0;
  if (typeof slot === 'number' || /^\d+$/.test(slot)) {
    const i = Number(slot) - 1;
    if (i < 0 || i >= printer.slots.length) throw new Error(`${printer.name} has slots 1 to ${printer.slots.length}.`);
    return i;
  }
  const i = printer.slots.findIndex((s) => s.id === slot || s.tray === slot);
  if (i < 0) throw new Error(`${printer.name} has no slot "${slot}". Slots: ${printer.slots.map((s, n) => `${n + 1} (${s.tray ?? s.id})`).join(', ')}.`);
  return i;
}

async function bedCentre(model: string): Promise<[number, number]> {
  try {
    const { printers } = await api.json<{ printers: { id: string; bed?: string }[] }>('/api/catalog/printers');
    const bed = printers.find((p) => p.id === model)?.bed?.split('x').map(Number);
    if (bed && bed[0] > 0 && bed[1] > 0) return [bed[0] / 2, bed[1] / 2];
  } catch {
  }
  return [128, 128];
}

function seconds(s: number | undefined) {
  if (!Number.isFinite(s)) return undefined;
  const t = Math.round(s!);
  const h = Math.floor(t / 3600), m = Math.floor((t % 3600) / 60), r = t % 60;
  return h ? `${h}h${m}m${r}s` : `${m}m${r}s`;
}

function summary(job: JobStatus) {
  const record = job.record ?? {};
  const out: Record<string, unknown> = { job: job.id, state: job.state, stage: job.stage, progress: job.progress, filename: job.filename };
  if (job.queue_position) out.queue_position = job.queue_position;
  if (job.state === 'failed' || job.state === 'canceled') Object.assign(out, { error: job.error, error_code: job.error_code });
  if (job.state !== 'done') return out;
  const stats = record.statistics ?? {};
  const time = record.tool?.estimated_time_s ?? record.totals?.estimated_time_s;
  Object.assign(out, {
    printer: record.inputs?.printer,
    base_process: record.inputs?.process,
    filaments_by_slot: record.inputs?.filaments,
    filament_presets: record.model_context?.presets?.filaments?.map((f: any) => `${f.slot}: ${f.name}`),
    estimated_time: seconds(time),
    estimated_time_s: time,
    weight_g: stats.total_weight_g !== undefined ? Math.round(stats.total_weight_g * 100) / 100 : undefined,
    filament_mm: stats.total_used_filament_mm !== undefined ? Math.round(stats.total_used_filament_mm) : undefined,
    per_extruder: stats.per_extruder,
    nonplanar: record.printed_flat?.map((p: any) => ({ object: p.name, method: p.method, flat_because: p.reason })),
    clearance_hit_moves: record.totals?.verifier?.hit_moves,
    fallbacks: record.tool?.fallback?.length ? record.tool.fallback : undefined,
  });
  return out;
}

tool('pancake_status', {
  title: 'PancakeSlicer status',
  description: 'Check that PancakeSlicer answers and report the engine state, queue and the printer connections it knows.',
  inputSchema: {},
  annotations: { readOnlyHint: true },
}, async () => {
  const health = await api.health();
  const live = await api.live(4000).catch(() => undefined);
  return reply({
    url: api.base,
    status: health.status,
    engine_state: health.engine_state,
    artifact_state: health.artifact_state,
    reason: health.reason || undefined,
    queued: health.queued,
    running: health.running,
    max_concurrency: health.max_concurrency,
    server_revision: health.server_revision,
    printer_connections: live?.connections.map((c) => ({ printer: c.printer, name: c.name, kind: c.kind, model: c.model })),
  });
});

tool('list_profiles', {
  title: 'List profiles',
  description: 'List the printers (with their filament slots and what is assigned to each), print processes and filaments in the PancakeSlicer library. Ids and names from here are what slice_model accepts.',
  inputSchema: {},
  annotations: { readOnlyHint: true },
}, async () => {
  const lib = await api.library();
  return reply({
    printers: lib.printers.map((p) => ({
      id: p.id, name: p.name, model: p.model, plate: p.plate, swap: p.swap,
      nozzles: p.nozzles?.map((n) => n.diameter),
      default_process: lib.processes.find((x) => x.id === p.process)?.name ?? p.process,
      slots: p.slots.map((s, i) => ({ slot: i + 1, tray: s.tray, filament: nameOf(lib, s.filament), filament_id: s.filament || undefined })),
    })),
    processes: lib.processes.map((p) => ({ id: p.id, name: p.name })),
    filaments: lib.filaments.map((f) => ({ id: f.id, name: f.name, based_on: f.product !== f.name ? f.product : undefined, overrides: f.settings ? Object.keys(f.settings) : undefined })),
  });
});

const settingValue = z.union([z.string(), z.number(), z.boolean(), z.array(z.string())]);

const normalise = (settings: Record<string, string | number | boolean | string[]> = {}) =>
  Object.fromEntries(Object.entries(settings).map(([k, v]) => [k, typeof v === 'boolean' ? (v ? '1' : '0') : Array.isArray(v) ? v : String(v)]));

function modelPath(raw: string) {
  const path = resolve(raw);
  if (!/\.(stl|obj|3mf|step|stp)$/i.test(path)) throw new Error('Models must be .stl, .obj, .3mf, .step or .stp files.');
  return path;
}

async function windowSlot(state: WindowState, slot: number | string | undefined) {
  if (slot === undefined || typeof slot === 'number' || /^\d+$/.test(slot)) return slot === undefined ? undefined : Number(slot);
  const lib = await api.library();
  return slotIndex(lib.printers.find((p) => p.name === state.printer) ?? pick(lib.printers, undefined, 'printer'), slot) + 1;
}

async function adjustWindow(args: { process?: string; slot?: number | string; filament?: string; settings?: Record<string, string | number | boolean | string[]>; layer_height?: number; force_planar?: boolean }) {
  let state = await inWindow('state');
  if (args.process) state = await inWindow('set_process', { process: args.process });
  const slot = await windowSlot(state, args.slot);
  if (args.filament) state = await inWindow('set_slot_filament', { slot: slot ?? 1, filament: args.filament });
  if (slot !== undefined) state = await inWindow('set_object_slot', { slot });
  const settings = normalise(args.settings);
  if (args.layer_height) settings.layer_height = String(args.layer_height);
  if (args.force_planar) settings.nonplanar_mode = 'off';
  if (Object.keys(settings).length) state = await inWindow('set_settings', { settings });
  return state;
}

async function sliceWindow(timeoutS: number) {
  const state = await inWindow('slice', { timeout_ms: timeoutS * 1000 });
  if (state.job.state !== 'done' || !state.job.id) return { window: state };
  const job = await api.waitJob(state.job.id, 120);
  return { ...summary(job), print_profile: state.process, objects: state.objects, filament_slots: state.filament_slots };
}

const adjustments = {
  process: z.string().optional().describe('Print process id or name (Draft, Everyday, Fine Detail, Strong or a user profile).'),
  layer_height: z.number().positive().max(2).optional().describe('Layer height in mm.'),
  slot: z.union([z.number().int().positive(), z.string()]).optional().describe('Filament slot the model prints with: 1-based number or tray name such as "ams0.2".'),
  filament: z.string().optional().describe('Filament profile id or name to put in that slot.'),
  settings: z.record(settingValue).optional().describe('Process setting overrides keyed by engine option name, for example {"wall_loops":"3","brim_type":"no_brim"}.'),
  force_planar: z.boolean().optional().describe('Slice with flat layers only.'),
};

tool('app_state', {
  title: 'What PancakeSlicer shows',
  description: 'Read the open PancakeSlicer window: project, plate objects and their filament slots, the selected printer, process and filaments, and the last slice.',
  inputSchema: {},
  annotations: { readOnlyHint: true },
}, async () => reply(await inWindow('state')));

tool('open_model', {
  title: 'Open a model in PancakeSlicer',
  description: 'Open an STL, OBJ, 3MF or STEP file from disk in the PancakeSlicer window, as a new project by default, so the user sees it on the plate.',
  inputSchema: {
    path: z.string().describe('Absolute path to the model file on this computer.'),
    new_project: z.boolean().default(true).describe('Start a new project for it; false adds it to the open plate.'),
  },
}, async ({ path, new_project }) => reply(await inWindow('open_model', { path: modelPath(path), new_project })));

tool('adjust_plate', {
  title: 'Change the plate settings',
  description: 'Change what is open in the PancakeSlicer window the way the user would: process, layer height, filament slot and its filament, and process settings. The edits stay in the open project. Slice again with slice_plate.',
  inputSchema: adjustments,
}, async (args) => reply(await adjustWindow(args)));

tool('slice_plate', {
  title: 'Slice the open plate',
  description: 'Press Slice plate in the PancakeSlicer window and wait; the window switches to the preview. Returns the job id, time, weight and profiles.',
  inputSchema: { wait_s: z.number().int().min(10).max(1800).default(600) },
}, async ({ wait_s }) => reply(await sliceWindow(wait_s)));

tool('slice_model', {
  title: 'Slice a model',
  description: 'Open one STL, OBJ, 3MF or STEP file from disk in the PancakeSlicer window as a new project, apply the process, slot, filament and settings, slice it and wait: the user watches it happen and ends on the preview. Returns the job id, estimated time, weight and the profiles used. background: true slices out of sight instead (also used for self-hosted servers), with settings as per-job overrides.',
  inputSchema: {
    path: z.string().describe('Absolute path to the model file on this computer.'),
    ...adjustments,
    printer: z.string().optional().describe('Printer profile id or name, for background slicing; the window slices with the printer it has selected.'),
    output: z.enum(['gcode.3mf', 'gcode']).default('gcode.3mf').describe('For background slicing: gcode.3mf for Bambu LAN printers, gcode for the others.'),
    background: z.boolean().default(false).describe('Slice without the window.'),
    wait_s: z.number().int().min(0).max(1800).default(600).describe('Seconds to wait for the slice.'),
  },
}, async (args) => {
  const path = modelPath(args.path);
  if (!(await stat(path)).isFile()) throw new Error(`${path} is not a file.`);
  if (!args.background && windowControl) {
    const opened = await inWindow('state');
    if (args.printer && args.printer !== opened.printer) throw new Error(`The window has ${opened.printer} selected. Ask the user to switch printers, or pass background: true.`);
    await inWindow('open_model', { path, new_project: true });
    await adjustWindow(args);
    return reply(await sliceWindow(Math.max(10, args.wait_s)));
  }
  const lib = await api.library();
  const printer = pick(lib.printers, args.printer, 'printer');
  const profile = pick(lib.processes, args.process ?? printer.process, 'process');
  const index = slotIndex(printer, args.slot);
  const filaments = printer.slots.map((s) => s.filament);
  if (args.filament) filaments[index] = pick(lib.filaments, args.filament, 'filament').id;
  if (!filaments[index]) throw new Error(`Slot ${index + 1} of ${printer.name} has no filament. Pass filament or assign one in PancakeSlicer.`);
  const project: Record<string, unknown> = {};
  if (args.settings) project.process = normalise(args.settings);
  const library = { printer: printer.id, print: profile.id, slots: printer.slots.map((s) => s.id), filaments, layerHeight: args.layer_height ?? 0, usedSlots: [index + 1], project };
  const form: Record<string, string> = { library: JSON.stringify(library), output: args.output };
  if (args.force_planar) form.force_planar = '1';
  if (index > 0 && extname(path).toLowerCase() !== '.3mf') {
    const [x, y] = await bedCentre(printer.model);
    form.plate = JSON.stringify({ objects: [{ file: basename(path), name: basename(path), extruder: index + 1, instances: [{ x, y, rot: 0 }] }] });
  }
  const id = await api.submitJob(path, form);
  const job = args.wait_s ? await api.waitJob(id, args.wait_s) : await api.job(id);
  return reply({ ...summary(job), background: true, print_profile: profile.name, slot: index + 1, tray: printer.slots[index].tray, filament: nameOf(lib, filaments[index]) });
});

tool('get_job', {
  title: 'Get a slice job',
  description: 'Report a slice job\'s state and, once done, its time, weight and profiles. Set wait_s to wait for a running job.',
  inputSchema: { job: z.string(), wait_s: z.number().int().min(0).max(1800).default(0) },
  annotations: { readOnlyHint: true },
}, async ({ job, wait_s }) => reply(summary(wait_s ? await api.waitJob(job, wait_s) : await api.job(job))));

tool('cancel_job', {
  title: 'Cancel a slice job',
  description: 'Cancel a queued or running slice job.',
  inputSchema: { job: z.string() },
}, async ({ job }) => {
  await api.cancelJob(job);
  return reply(summary(await api.job(job)));
});

tool('download_job', {
  title: 'Save sliced output',
  description: 'Save a finished job\'s G-code or G-code 3MF to disk. path may be a folder (the job\'s own file name is used) or a full file path.',
  inputSchema: { job: z.string(), path: z.string().describe('Absolute folder or file path.') },
}, async ({ job, path }) => {
  if (!isAbsolute(path)) throw new Error('Give an absolute path.');
  const { data, filename } = await api.download(job);
  let target = resolve(path);
  const existing = await stat(target).catch(() => undefined);
  if (existing?.isDirectory() || /[\\/]$/.test(path)) target = join(target, filename);
  await mkdir(dirname(target), { recursive: true });
  await writeFile(target, data);
  return reply({ saved: target, bytes: data.length });
});

tool('inspect_gcode', {
  title: 'Inspect sliced G-code',
  description: 'Read a finished job\'s G-code: the printer, process and filament presets, nozzle and bed temperatures actually set, support and wall settings, and for a G-code 3MF the slice_info (filament type and tray_info_idx per used slot, warnings such as bed_temperature_too_high_than_filament).',
  inputSchema: { job: z.string() },
  annotations: { readOnlyHint: true },
}, async ({ job }) => {
  const { data, filename } = await api.download(job);
  return reply({ filename, ...inspectOutput(data, filename) });
});

async function printerFor(wanted?: string) {
  const live = await api.live();
  if (!live.enabled) throw new Error('Printer connections are switched off on this PancakeSlicer.');
  if (!live.connections.length) throw new Error('PancakeSlicer has no printer connections. Add the printer in its Device tab first.');
  const lower = wanted?.toLowerCase();
  const connection = !wanted ? live.connections[0]
    : live.connections.find((c) => c.printer === wanted || c.id === wanted || c.name.toLowerCase() === lower)
      ?? live.connections.find((c) => c.name.toLowerCase().includes(lower!));
  if (!connection) throw new Error(`No printer connection "${wanted}". Known: ${live.connections.map((c) => c.name).join(', ')}.`);
  return { connection, status: live.status?.find((s) => s.printer === connection.printer), live };
}

tool('printer_status', {
  title: 'Printer status',
  description: 'Live status of a connected printer: state, current file and layer, temperatures, chamber light, and the AMS or external spool trays with the filament type the printer reports. Also lists recent transfers to it.',
  inputSchema: { printer: z.string().optional().describe('Printer profile id, connection id or name. Defaults to the first connection.') },
  annotations: { readOnlyHint: true },
}, async ({ printer }) => {
  const { connection, status, live } = await printerFor(printer);
  return reply({
    connection: { printer: connection.printer, name: connection.name, kind: connection.kind, model: connection.model, host: connection.host },
    status: status ? {
      link: status.link, online: status.online, state: status.state, detail: status.detail, file: status.file,
      layer: status.layer, layers: status.layers, progress: status.progress, remaining_min: status.remaining,
      nozzle: status.nozzle, bed: status.bed, chamber: status.chamber, light: status.light, speed_mode: status.speed_mode,
      trays: status.trays?.map((t: any) => ({ kind: t.kind, tray: t.kind === 'external' ? 'external' : `ams${t.ams}.${t.slot}`, type: t.type, name: t.name, color: t.color, remaining: t.remaining, active: t.active || undefined })),
      errors: status.hms ?? status.errors,
    } : 'No live status yet.',
    recent_transfers: live.transfers?.filter((t) => t.printer === connection.printer).slice(0, 5).map((t) => ({ job: t.job, filename: t.filename, state: t.state, outcome: t.outcome, message: t.message, started: t.started })),
  });
});

tool('printer_light', {
  title: 'Chamber light',
  description: 'Switch the printer\'s chamber light on or off, for example before camera_snapshot.',
  printers: true,
  inputSchema: { on: z.boolean(), printer: z.string().optional() },
}, async ({ on, printer }) => {
  const { connection } = await printerFor(printer);
  return reply(await api.device(connection.printer, { control: 'light', value: on ? 1 : 0 }));
});

tool('camera_snapshot', {
  title: 'Camera snapshot',
  description: 'Grab one frame from the printer\'s camera, for example to check that the build plate is empty before sending a job. Turn the light on first for a usable picture.',
  printers: true,
  inputSchema: { printer: z.string().optional(), camera: z.string().optional().describe('Camera id; defaults to the first.') },
  annotations: { readOnlyHint: true },
}, async ({ printer, camera }) => {
  const { connection } = await printerFor(printer);
  const { cameras } = await api.cameras(connection.printer);
  const chosen = camera ? cameras.find((c) => c.id === camera || c.name === camera) : cameras[0];
  if (!chosen) throw new Error(`No camera ${camera ?? ''} on ${connection.name}. Cameras: ${cameras.map((c) => `${c.id} (${c.name})`).join(', ') || 'none'}.`);
  if (chosen.format !== 'mjpeg') throw new Error(`${chosen.name} streams ${chosen.format}; only MJPEG cameras can be snapshotted here.`);
  const frame = await api.cameraFrame(connection.printer, chosen.id);
  return reply(`${connection.name}, ${chosen.name}, ${new Date().toISOString()}`, [{ type: 'image', data: Buffer.from(frame).toString('base64'), mimeType: 'image/jpeg' }]);
});

tool('send_plan', {
  title: 'Preview a send',
  description: 'Show how a finished job\'s filament slots map to the Bambu printer\'s trays, with the filament each slot expects and what the printer reports in that tray. A used slot whose types differ would print the wrong material.',
  inputSchema: { job: z.string(), printer: z.string().optional() },
  annotations: { readOnlyHint: true },
}, async ({ job, printer }) => {
  const { connection } = await printerFor(printer);
  const plan = await api.sendPlan(job, connection.printer);
  const mismatches = plan.slots.filter((s) => s.used && s.expected?.type && s.reported?.type && s.expected.type !== s.reported.type);
  return reply({ printer: connection.name, ...plan, mismatches: mismatches.length ? mismatches.map((s) => `slot ${s.filament} (${s.tray}): job expects ${s.expected.type}, printer has ${s.reported.type}`) : undefined });
});

tool('send_job', {
  title: 'Send a job to the printer',
  description: 'Upload a finished job to a connected printer and start it, or upload only (start=false) so the user starts it on the printer. Only call this after the user has explicitly approved this exact job; set confirm=true to show that. Check send_plan and the plate first.',
  printers: true,
  inputSchema: {
    job: z.string(),
    confirm: z.literal(true).describe('Must be true: the user approved sending this job.'),
    printer: z.string().optional(),
    start: z.boolean().default(true).describe('false uploads without starting.'),
    filename: z.string().optional().describe('Name on the printer; must end with the job\'s extension.'),
    bed_leveling: z.boolean().optional(),
    timelapse: z.boolean().default(false),
    flow_cali: z.boolean().default(false),
    vibration_cali: z.boolean().optional(),
  },
  annotations: { destructiveHint: true },
}, async ({ job, printer, start, filename, bed_leveling, timelapse, flow_cali, vibration_cali }) => {
  const { connection } = await printerFor(printer);
  const body: Record<string, unknown> = { printer: connection.printer, start, timelapse, flow_cali };
  if (filename) body.filename = filename;
  if (bed_leveling !== undefined) body.bed_leveling = bed_leveling;
  if (vibration_cali !== undefined) body.vibration_cali = vibration_cali;
  await api.send(job, body);
  let state: Record<string, any> = {};
  for (let i = 0; i < 600; i++) {
    state = await api.sendState(job);
    if (state.state !== 'uploading' && state.state !== 'starting') break;
    await new Promise((r) => setTimeout(r, 1000));
  }
  const { sent, total, ...rest } = state;
  return reply({ ...rest, uploaded: total ? `${Math.round((sent / total) * 100)}%` : undefined });
});

tool('printer_control', {
  title: 'Pause, resume or stop a print',
  description: 'Pause, resume or stop the current print. Only with the user\'s explicit go-ahead; stop cannot be undone.',
  printers: true,
  inputSchema: { action: z.enum(['pause', 'resume', 'stop']), confirm: z.literal(true), printer: z.string().optional() },
  annotations: { destructiveHint: true },
}, async ({ action, printer }) => {
  const { connection } = await printerFor(printer);
  return reply(await api.control(connection.printer, action));
});

server.connect(new StdioServerTransport()).catch((error) => {
  console.error(error);
  process.exit(1);
});
