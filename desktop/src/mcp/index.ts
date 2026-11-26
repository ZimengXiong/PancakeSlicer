import { mkdir, readFile, stat, writeFile } from 'node:fs/promises';
import { basename, dirname, extname, isAbsolute, join, resolve } from 'node:path';
import { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js';
import { StdioServerTransport } from '@modelcontextprotocol/sdk/server/stdio.js';
import { z } from 'zod';
import { ApiError, MAX_TIMER_SECONDS, PancakeApi, statusWait, waitForStatus, type JobStatus, type LibraryDocument, type LibraryPrinter, type ProfileRef } from './api.js';
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
  status?: 'dialog_pending' | 'choice_required';
  dialog?: { title: string; message?: string; options: { id: string; label: string }[]; choices?: string[]; retry_parameter?: string; open?: boolean };
  tab: string; project: string | null; plate: number; printer: string; process: string;
  document_generation: number; slice_generation: number;
  filament_slots: { slot: number; filament: string }[];
  objects: { id: number; name: string; slot: number }[];
  job: { id?: string; state: string; stage?: string; progress?: number; stale?: boolean; error?: string; error_code?: string };
}

function needWindow() {
  if (!windowControl) throw new Error('This needs the PancakeSlicer desktop window, which is not connected. Use background: true to slice without it.');
  return windowControl;
}

async function inWindow(command: string, args: Record<string, unknown> = {}, signal?: AbortSignal): Promise<WindowState> {
  const control = needWindow();
  let response: Response;
  try {
    response = await fetch(`${control.url}/command`, {
      method: 'POST', headers: { Authorization: `Bearer ${control.token}`, 'Content-Type': 'application/json' }, body: JSON.stringify({ command, args }), signal,
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
    'Slicing and uploading to a printer can take several minutes. Set wait_s to fit the MCP client timeout. If a slice is still running, poll its job ID with get_job and a short wait_s. If the upload has not returned a job ID yet, use app_state to find it. Then inspect_gcode when the filament or temperatures matter.',
    'If a tool returns dialog_pending, ask the user to respond in PancakeSlicer, then check app_state. The window command can continue after the dialog closes. Follow any next instructions in the result.',
    'If a tool returns choice_required, the dialog has closed. After the user chooses, retry with the given retry_parameter and option ID, or stop if the user chooses Cancel. Follow any next instructions in the result.',
    'Before send_job: printer_status, then printer_light on and camera_snapshot to check that the plate is empty, then send_plan to read the filament to tray mapping. Use the same printer ID from printer_status for the checks, approval and send.',
    'If printer_status reports a saved credential problem, explain it and direct the user to Device › Connection settings to update the credential privately.',
    'Never call send_job or printer_control without the user\'s explicit go-ahead for that exact job or action; approval for one job does not cover the next.',
    'A send_plan slot whose expected and reported filament types differ means the job would pull the wrong material. Stop and tell the user rather than sending.',
  ].join(' '),
});

type Content = { type: 'text'; text: string } | { type: 'image'; data: string; mimeType: string };

function reply(value: unknown, extra: Content[] = []) {
  const text = typeof value === 'string' ? value : JSON.stringify(value, null, 2);
  const pending = value && typeof value === 'object' && 'status' in value && (value.status === 'dialog_pending' || value.status === 'choice_required');
  return { content: [{ type: 'text' as const, text }, ...extra], ...(pending ? { structuredContent: value } : {}) };
}

function failure(error: unknown) {
  const e = error as ApiError;
  const detail = e instanceof ApiError && e.body && typeof e.body === 'object' ? `\n${JSON.stringify(e.body, null, 2)}` : '';
  return { isError: true, content: [{ type: 'text' as const, text: `${e.message ?? String(error)}${detail}` }] };
}

function tool<T extends z.ZodRawShape>(name: string, config: { title: string; description: string; inputSchema: T; annotations?: Record<string, boolean>; printers?: boolean },
  run: (args: z.objectOutputType<T, z.ZodTypeAny>, signal: AbortSignal) => Promise<{ content: Content[] }>) {
  const { printers, ...definition } = config;
  server.registerTool(name, definition, (async (args: z.objectOutputType<T, z.ZodTypeAny>, { signal }: { signal: AbortSignal }) => {
    try {
      await connect();
      if (printers) needPrinterAccess();
      return await run(args, signal);
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

async function bedCenter(printer: LibraryPrinter, selection: string): Promise<[number, number]> {
  const { printer: resolved } = await api.resolvePrinter(selection);
  if (!resolved.status.ok) throw new Error(`Could not resolve the bed for ${printer.name}: ${resolved.status.message ?? 'unknown printer settings'}.`);
  const area = resolved.values?.printable_area;
  const coordinates = typeof area === 'string' ? area.split(/[,;]/) : area ?? [];
  const points = coordinates.map((point) => point.split('x').map((value) => value.trim() ? Number(value) : NaN));
  if (points.length < 3 || points.some((point) => point.length !== 2 || !point.every(Number.isFinite))) {
    throw new Error(`${printer.name} has no valid printable area for model placement.`);
  }
  const xs = points.map(([x]) => x);
  const ys = points.map(([, y]) => y);
  return [(Math.min(...xs) + Math.max(...xs)) / 2, (Math.min(...ys) + Math.max(...ys)) / 2];
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
  if (state.status === 'dialog_pending') return state;
  if (args.process) {
    state = await inWindow('set_process', { process: args.process });
    if (state.status === 'dialog_pending') return state;
  }
  const slot = await windowSlot(state, args.slot);
  if (args.filament) {
    state = await inWindow('set_slot_filament', { slot: slot ?? 1, filament: args.filament });
    if (state.status === 'dialog_pending') return state;
  }
  if (slot !== undefined) {
    state = await inWindow('set_object_slot', { slot });
    if (state.status === 'dialog_pending') return state;
  }
  const settings = normalise(args.settings);
  if (args.layer_height) settings.layer_height = String(args.layer_height);
  if (args.force_planar) settings.nonplanar_mode = 'off';
  if (Object.keys(settings).length) state = await inWindow('set_settings', { settings });
  return state;
}

const sliceChoices = {
  missing_slots: z.enum(['remap', 'cancel']).optional().describe('When a used filament slot is unavailable, remap it to an available slot or cancel. Omit to return the choices.'),
  native_support_conflict: z.enum(['continue', 'cancel']).optional().describe('When native supports conflict with plate settings, slice anyway or cancel. Omit to return the choices.'),
};

const waitSeconds = z.number().int().nonnegative().max(MAX_TIMER_SECONDS).optional()
  .describe('Seconds to wait for completion after starting or looking up the operation. Omit for no server-side wait limit. Set to 0 to return the current status. The MCP client may have its own timeout.');

async function sliceWindow(waitS: number | undefined, choices: { missing_slots?: 'remap' | 'cancel'; native_support_conflict?: 'continue' | 'cancel' }, signal: AbortSignal) {
  const started = await inWindow('slice', { missing_slots: choices.missing_slots, native_support_conflict: choices.native_support_conflict }, signal);
  if (started.status === 'choice_required') return {
    ...started, next: 'Retry with slice_plate using the indicated parameter and selected option, or stop if the user chooses Cancel.',
  };
  if (started.status === 'dialog_pending') return started;

  const wait = statusWait(waitS, signal);
  const state = await waitForStatus(started, async (statusSignal) => {
    const current = await inWindow('state', {}, statusSignal);
    if (current.document_generation !== started.document_generation || current.plate !== started.plate || current.slice_generation !== started.slice_generation) {
      throw new Error('The window changed before the slice received a job ID. Check app_state for the current plate.');
    }
    return current;
  }, ({ job }) => job.state === 'submitting' && !job.id, wait);
  if (!state.job.id || wait.signal.aborted && state.job.state !== 'done') return {
    job: state.job.id,
    window: state,
    next: state.job.state === 'submitting' ? 'Use app_state to find the job ID after upload.'
      : state.job.state === 'running' ? 'Poll this job with get_job.' : undefined,
  };
  const job = await api.waitJob(state.job.id, wait);
  if (job.state === 'queued' || job.state === 'running') return { ...summary(job), window: state, next: 'Poll this job with get_job.' };
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

const import3mf = z.enum(['objects_only', 'objects_and_settings']).optional()
  .describe('Choose how to import a 3MF in the desktop window. Omit to return the available choices. If the archive contains no settings, either choice imports objects only.');

tool('open_model', {
  title: 'Open a model in PancakeSlicer',
  description: 'Open an STL, OBJ, 3MF or STEP file from disk in the PancakeSlicer window, as a new project by default, so the user sees it on the plate.',
  inputSchema: {
    path: z.string().describe('Absolute path to the model file on this computer.'),
    new_project: z.boolean().default(true).describe('Start a new project for it; false adds it to the open plate.'),
    import_3mf: import3mf,
  },
}, async ({ path, new_project, import_3mf }) => reply(await inWindow('open_model', { path: modelPath(path), new_project, import_3mf })));

tool('adjust_plate', {
  title: 'Change the plate settings',
  description: 'Change what is open in the PancakeSlicer window the way the user would: process, layer height, filament slot and its filament, and process settings. The edits stay in the open project. Slice again with slice_plate.',
  inputSchema: adjustments,
}, async (args) => reply(await adjustWindow(args)));

tool('slice_plate', {
  title: 'Slice the open plate',
  description: 'Slice the open plate. Slicing can take several minutes. If wait_s elapses, the slice continues and the tool returns the job ID when available.',
  inputSchema: { ...sliceChoices, wait_s: waitSeconds },
}, async (args, signal) => reply(await sliceWindow(args.wait_s, args, signal)));

tool('slice_model', {
  title: 'Slice a model',
  description: 'Open one STL, OBJ, 3MF or STEP file as a new project, apply the settings, and slice it. Slicing can take several minutes. Use wait_s to limit the wait and get_job to check an unfinished slice. background: true slices without the window, as on self-hosted servers.',
  inputSchema: {
    path: z.string().describe('Absolute path to the model file on this computer.'),
    ...adjustments,
    import_3mf: import3mf,
    ...sliceChoices,
    printer: z.string().optional().describe('Printer profile id or name, for background slicing; the window slices with the printer it has selected.'),
    output: z.enum(['gcode.3mf', 'gcode']).default('gcode.3mf').describe('For background slicing: gcode.3mf for Bambu LAN printers, gcode for the others.'),
    background: z.boolean().default(false).describe('Slice without the window.'),
    wait_s: waitSeconds,
  },
}, async (args, signal) => {
  const path = modelPath(args.path);
  if (!(await stat(path)).isFile()) throw new Error(`${path} is not a file.`);
  if (!args.background && windowControl) {
    const opened = await inWindow('state');
    if (opened.status === 'dialog_pending') return reply(opened);
    if (args.printer && args.printer !== opened.printer) throw new Error(`The window has ${opened.printer} selected. Ask the user to switch printers, or pass background: true.`);
    const imported = await inWindow('open_model', { path, new_project: true, import_3mf: args.import_3mf });
    if (imported.status === 'choice_required') return reply(imported);
    if (imported.status === 'dialog_pending') return reply({
      ...imported,
      next: 'After the user responds, check app_state for the open_model result. If the model opened, use adjust_plate with the requested settings and then slice_plate.',
    });
    const adjusted = await adjustWindow(args);
    if (adjusted.status === 'dialog_pending') return reply({
      ...adjusted,
      next: 'After the user responds, check app_state, apply any remaining settings with adjust_plate, and then use slice_plate.',
    });
    return reply(await sliceWindow(args.wait_s, args, signal));
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
    const [x, y] = await bedCenter(printer, form.library);
    form.plate = JSON.stringify({ objects: [{ file: basename(path), name: basename(path), extruder: index + 1, instances: [{ x, y, rot: 0 }] }] });
  }
  const id = await api.submitJob(path, form);
  const job = await api.waitJob(id, statusWait(args.wait_s, signal));
  return reply({ ...summary(job), background: true, print_profile: profile.name, slot: index + 1, tray: printer.slots[index].tray, filament: nameOf(lib, filaments[index]) });
});

tool('get_job', {
  title: 'Get a slice job',
  description: 'Report a slice job’s state and its results when done. An active slice can take several minutes. Set wait_s to 0 for a status check.',
  inputSchema: { job: z.string(), wait_s: waitSeconds },
  annotations: { readOnlyHint: true },
}, async ({ job, wait_s }, signal) => reply(summary(await api.waitJob(job, statusWait(wait_s, signal)))));

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

async function printerFor(wanted?: string, selector: 'id-or-name' | 'printer-id' = 'id-or-name') {
  const live = await api.live();
  if (!live.enabled) throw new Error('Printer connections are switched off on this PancakeSlicer.');
  const connections = live.connections.filter((connection) => connection.printer);
  if (!connections.length) throw new Error('No printer connection is assigned to a profile. Set up a printer in the Device tab first.');

  const describe = (entries: typeof connections) => entries.map((c) => `${c.name} (${c.printer})`).join(', ');
  let matches = connections;
  if (wanted !== undefined) {
    matches = connections.filter((c) => c.printer === wanted);
    if (!matches.length && selector === 'id-or-name') {
      const name = wanted.toLowerCase();
      matches = connections.filter((c) => c.id === wanted);
      if (!matches.length) matches = connections.filter((c) => c.name.toLowerCase() === name);
      if (!matches.length) matches = connections.filter((c) => c.name.toLowerCase().includes(name));
    }
  }
  if (!matches.length) throw new Error(`No printer connection "${wanted}". Printer IDs: ${describe(connections)}.`);
  if (matches.length > 1) {
    const message = wanted === undefined ? 'Several printers are connected.' : `"${wanted}" matches several printer connections.`;
    throw new Error(`${message} Choose a printer ID: ${describe(matches)}.`);
  }
  const [connection] = matches;
  return { connection, status: live.status?.find((s) => s.printer === connection.printer), live };
}

const printerSelector = z.string().trim().min(1).optional()
  .describe('Printer profile ID, connection ID, or unique name. Optional when only one printer profile is connected.');
const printerId = z.string().trim().min(1)
  .describe('Exact printer profile ID from connection.printer in printer_status for the printer the user approved.');

tool('printer_status', {
  title: 'Printer status',
  description: 'Live status of a connected printer: state, current file and layer, temperatures, chamber light, and the AMS or external spool trays with the filament type the printer reports. Also lists recent transfers to it.',
  inputSchema: { printer: printerSelector },
  annotations: { readOnlyHint: true },
}, async ({ printer }) => {
  const { connection, status, live } = await printerFor(printer);
  return reply({
    connection: { printer: connection.printer, id: connection.id, name: connection.name, kind: connection.kind, model: connection.model, host: connection.host },
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
  inputSchema: { on: z.boolean(), printer: printerSelector },
}, async ({ on, printer }) => {
  const { connection } = await printerFor(printer);
  return reply(await api.device(connection.printer, { control: 'light', value: on ? 1 : 0 }));
});

tool('camera_snapshot', {
  title: 'Camera snapshot',
  description: 'Grab one frame from the printer\'s camera, for example to check that the build plate is empty before sending a job. Turn the light on first for a usable picture.',
  printers: true,
  inputSchema: { printer: printerSelector, camera: z.string().optional().describe('Camera id; defaults to the first.') },
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
  inputSchema: { job: z.string(), printer: printerSelector },
  annotations: { readOnlyHint: true },
}, async ({ job, printer }) => {
  const { connection } = await printerFor(printer);
  const plan = await api.sendPlan(job, connection.printer);
  const mismatches = plan.slots.filter((s) => s.used && s.expected?.type && s.reported?.type && s.expected.type !== s.reported.type);
  return reply({ printer: connection.name, printer_id: connection.printer, ...plan, mismatches: mismatches.length ? mismatches.map((s) => `slot ${s.filament} (${s.tray}): job expects ${s.expected.type}, printer has ${s.reported.type}`) : undefined });
});

tool('send_job', {
  title: 'Send a job to the printer',
  description: 'Upload a finished job to a connected printer and start it, or upload only (start=false). Uploading can take several minutes. If wait_s elapses, the transfer continues. Check printer_status for progress. Set confirm=true only after the user explicitly approves this job on this printer. Check send_plan and the plate first.',
  printers: true,
  inputSchema: {
    job: z.string(),
    confirm: z.literal(true).describe('Must be true: the user approved sending this job.'),
    printer: printerId,
    start: z.boolean().default(true).describe('false uploads without starting.'),
    filename: z.string().optional().describe('Name on the printer; must end with the job\'s extension.'),
    bed_leveling: z.boolean().optional(),
    timelapse: z.boolean().default(false),
    flow_cali: z.boolean().default(false),
    vibration_cali: z.boolean().optional(),
    wait_s: waitSeconds,
  },
  annotations: { destructiveHint: true },
}, async ({ job, printer, start, filename, bed_leveling, timelapse, flow_cali, vibration_cali, wait_s }, signal) => {
  const { connection } = await printerFor(printer, 'printer-id');
  const body: Record<string, unknown> = { printer: connection.printer, start, timelapse, flow_cali };
  if (filename) body.filename = filename;
  if (bed_leveling !== undefined) body.bed_leveling = bed_leveling;
  if (vibration_cali !== undefined) body.vibration_cali = vibration_cali;
  const accepted = await api.send(job, body);
  const state = await waitForStatus(accepted, async (statusSignal) => {
    const current = await api.sendState(job, statusSignal);
    if (current.id !== accepted.id) throw new Error('A different transfer is now recorded for this job. Check printer_status.');
    return current;
  }, ({ state }) => state === 'uploading' || state === 'starting', statusWait(wait_s, signal));
  const { sent, total, ...rest } = state;
  return reply({ ...rest, uploaded: total ? `${Math.round((sent / total) * 100)}%` : undefined });
});

tool('printer_control', {
  title: 'Pause, resume or stop a print',
  description: 'Pause, resume or stop the current print. Only with the user\'s explicit go-ahead; stop cannot be undone.',
  printers: true,
  inputSchema: { action: z.enum(['pause', 'resume', 'stop']), confirm: z.literal(true), printer: printerId },
  annotations: { destructiveHint: true },
}, async ({ action, printer }) => {
  const { connection } = await printerFor(printer, 'printer-id');
  return reply(await api.control(connection.printer, action));
});

server.connect(new StdioServerTransport()).catch((error) => {
  console.error(error);
  process.exit(1);
});
