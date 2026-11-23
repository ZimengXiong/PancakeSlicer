import { importFiles, newProject, slice } from './actions';
import { setFilament } from './edit';
import { slotFilaments } from './profiles/slots';
import type { ConfigValue } from './api/types';
import { getState, setOption, subscribeStore, type State } from './state/store';

type Args = Record<string, unknown>;

const named = <T extends { id: string; name: string }>(list: T[], wanted: string, kind: string) => {
  const lower = wanted.toLowerCase();
  const found = list.find((r) => r.id === wanted) ?? list.find((r) => r.name.toLowerCase() === lower)
    ?? list.find((r) => r.name.toLowerCase().includes(lower));
  if (!found) throw new Error(`No ${kind} called "${wanted}". Known: ${list.map((r) => r.name).join(', ')}.`);
  return found;
};

function view(s: State = getState()) {
  const name = (list: { id: string; name: string }[], id: string) => list.find((r) => r.id === id)?.name ?? id;
  const { records, job } = s;
  return {
    tab: s.tab,
    project: s.projectName,
    plate: s.plateIndex,
    printer: name(records.printers, s.printer),
    process: name(records.processes, s.process),
    filament_slots: slotFilaments(s).map((id, i) => ({ slot: i + 1, filament: id ? name(records.filaments, id) : '(empty)' })),
    objects: s.objects.map((o) => ({ id: o.id, name: o.name, slot: o.extruder })),
    job: { id: job.id, state: job.state, stage: job.stage, progress: job.pct, stale: job.stale || undefined, error: job.error, error_code: job.errorCode },
  };
}

const settled = (s: State) => s.job.state === 'done' || s.job.state === 'failed' || s.job.state === 'canceled';

function waitForJob(timeoutMs: number) {
  return new Promise<void>((resolve) => {
    const timer = setTimeout(finish, timeoutMs);
    const stop = subscribeStore(() => { if (settled(getState())) finish(); });
    function finish() { clearTimeout(timer); stop(); resolve(); }
    if (settled(getState())) finish();
  });
}

async function run(command: string, args: Args) {
  switch (command) {
    case 'state':
      return view();
    case 'open_model': {
      if (args.new_project !== false && !(await newProject(true))) throw new Error('The user kept the current plate, so the model was not opened.');
      const data = args.data as Uint8Array;
      const file = new File([data.slice().buffer as ArrayBuffer], String(args.name), { lastModified: Date.now() });
      if (!(await importFiles([file]))) throw new Error(`PancakeSlicer could not import ${String(args.name)}.`);
      return view();
    }
    case 'set_process': {
      const record = named(getState().records.processes, String(args.process), 'process');
      const { requestPreset } = await import('./components/PresetCombo');
      await requestPreset('process', 0, record.id);
      return view();
    }
    case 'set_slot_filament': {
      const slot = Number(args.slot);
      const record = named(getState().records.filaments, String(args.filament), 'filament');
      const { requestPreset } = await import('./components/PresetCombo');
      await requestPreset('filament', slot - 1, record.id);
      return view();
    }
    case 'set_object_slot': {
      const slot = Number(args.slot);
      const ids = args.object === undefined ? getState().objects.map((o) => o.id) : [Number(args.object)];
      for (const id of ids) setFilament(id, slot);
      return view();
    }
    case 'set_settings': {
      for (const [key, value] of Object.entries((args.settings ?? {}) as Record<string, ConfigValue>)) setOption('process', key, value);
      return view();
    }
    case 'slice': {
      if (!getState().objects.length) throw new Error('The plate is empty: open a model first.');
      await slice();
      const started = getState().job.state;
      if (started === 'idle' || started === 'failed' && getState().job.errorCode === 'slice_refused') return view();
      await waitForJob(Number(args.timeout_ms) || 600_000);
      return view();
    }
  }
  throw new Error(`Unknown command ${command}.`);
}

export function startAgentBridge() {
  const agents = window.orcaDesktop?.agents;
  if (!agents?.onCommand) return;
  agents.onCommand((id, command, args) => {
    run(command, args).then((result) => agents.reply(id, true, result), (error: unknown) => agents.reply(id, false, error instanceof Error ? error.message : String(error)));
  });
}
