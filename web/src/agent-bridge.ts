import { getSliceGeneration, importFiles, newProject, slice } from './actions';
import { setFilament } from './edit';
import { slotFilaments } from './profiles/slots';
import type { ConfigValue } from './api/types';
import { getState, setOption, type State } from './state/store';
import { documentGeneration } from './state/history';
import { pendingAsk, subscribeAsk } from './components/Ask';

type CommandArgs = Record<string, unknown>;
type RequiredChoice = {
  title: string;
  message: string;
  options: { id: string; label: string }[];
  retry_parameter: 'import_3mf' | 'missing_slots' | 'native_support_conflict';
};

let operation: { command: string; status: 'running' | 'completed' | 'failed'; error?: string } | undefined;
let commandRunning = false;

function resolveProfile<T extends { id: string; name: string }>(list: T[], wanted: string, kind: string) {
  const lower = wanted.toLowerCase();
  const exact = list.find((r) => r.id === wanted) ?? list.find((r) => r.name.toLowerCase() === lower);
  if (exact) return exact;
  const matches = list.filter((r) => r.name.toLowerCase().includes(lower));
  if (matches.length > 1) throw new Error(`"${wanted}" matches several ${kind}s: ${matches.map((r) => r.name).join(', ')}.`);
  if (!matches.length) throw new Error(`No ${kind} called "${wanted}". Known: ${list.map((r) => r.name).join(', ')}.`);
  return matches[0];
}

function getWindowState(s: State = getState()) {
  const name = (list: { id: string; name: string }[], id: string) => list.find((r) => r.id === id)?.name ?? id;
  const { records, job } = s;
  const dialog = pendingAsk();
  return {
    ...(dialog ? { status: 'dialog_pending', dialog } : {}),
    ...(operation ? { operation: { ...operation } } : {}),
    tab: s.tab,
    project: s.projectName,
    plate: s.plateIndex,
    document_generation: documentGeneration(),
    slice_generation: getSliceGeneration(),
    printer: name(records.printers, s.printer),
    process: name(records.processes, s.process),
    filament_slots: slotFilaments(s).map((id, i) => ({ slot: i + 1, filament: id ? name(records.filaments, id) : '(empty)' })),
    objects: s.objects.map((o) => ({ id: o.id, name: o.name, slot: o.extruder })),
    job: { id: job.id, state: job.state, stage: job.stage, progress: job.pct, stale: job.stale || undefined, error: job.error, error_code: job.errorCode },
  };
}

function choiceRequired(dialog: RequiredChoice) {
  return { ...getWindowState(), status: 'choice_required', dialog: { ...dialog, open: false } };
}

async function runWindowCommand(command: string, args: CommandArgs) {
  switch (command) {
    case 'open_model': {
      const { printer, process } = getState();
      if (!printer || !process) throw new Error('Choose a printer and print profile in PancakeSlicer before opening a model.');
      const data = args.data as Uint8Array;
      const file = new File([data.slice().buffer as ArrayBuffer], String(args.name), { lastModified: Date.now() });
      let imported: number;
      if (/\.3mf$/i.test(file.name)) {
        const { importObjects } = await import('./plater/import-project');
        let required: RequiredChoice | undefined;
        imported = await importObjects(file, {
          newProject: args.new_project !== false,
          choose({ title, message, primary, alternative }) {
            if (args.import_3mf === 'objects_and_settings') return primary.id;
            if (args.import_3mf === 'objects_only') return 'import';
            const options = [{ id: primary.id === 'settings' ? 'objects_and_settings' : 'objects_only', label: primary.label }];
            if (alternative) options.push({ id: 'objects_only', label: alternative.label });
            options.push({ id: 'cancel', label: 'Cancel' });
            required = { title, message, options, retry_parameter: 'import_3mf' };
            return null;
          },
        });
        if (required) return choiceRequired(required);
      } else {
        if (args.new_project !== false && !(await newProject(true))) throw new Error('The user kept the current plate, so the model was not opened.');
        imported = await importFiles([file]);
      }
      if (!imported) throw new Error(`PancakeSlicer could not import ${file.name}.`);
      return getWindowState();
    }
    case 'set_process': {
      const record = resolveProfile(getState().records.processes, String(args.process), 'process');
      const { requestPreset } = await import('./components/PresetCombo');
      await requestPreset('process', 0, record.id);
      return getWindowState();
    }
    case 'set_slot_filament': {
      const slot = Number(args.slot);
      const record = resolveProfile(getState().records.filaments, String(args.filament), 'filament');
      const { requestPreset } = await import('./components/PresetCombo');
      await requestPreset('filament', slot - 1, record.id);
      return getWindowState();
    }
    case 'set_object_slot': {
      const slot = Number(args.slot);
      const ids = args.object === undefined ? getState().objects.map((o) => o.id) : [Number(args.object)];
      for (const id of ids) setFilament(id, slot);
      return getWindowState();
    }
    case 'set_settings': {
      for (const [key, value] of Object.entries((args.settings ?? {}) as Record<string, ConfigValue>)) setOption('process', key, value);
      return getWindowState();
    }
    case 'slice': {
      if (!getState().objects.length) throw new Error('The plate is empty: open a model first.');
      let required: RequiredChoice | undefined;
      const result = await slice(undefined, ({ kind, title, message, buttons = ['Continue', 'Cancel'] }) => {
        const [parameter, option] = kind === 'remapSlots' ? ['missing_slots', 'remap'] as const : ['native_support_conflict', 'continue'] as const;
        if (args[parameter] === option) return true;
        if (args[parameter] !== 'cancel') required = {
          title, message, retry_parameter: parameter,
          options: [{ id: option, label: buttons[0] }, { id: 'cancel', label: buttons[1] }],
        };
        return false;
      });
      if (required) return choiceRequired(required);
      if (result === false) throw new Error('Slicing was canceled before submission.');
      return getWindowState();
    }
  }
  throw new Error(`Unknown command ${command}.`);
}

export function startAgentBridge() {
  const agents = window.orcaDesktop?.agents;
  if (!agents?.onCommand) return;
  agents.onCommand((id, command, args) => {
    if (command === 'state' || pendingAsk()) {
      agents.reply(id, true, getWindowState());
      return;
    }
    if (commandRunning) {
      agents.reply(id, false, 'A PancakeSlicer command is still running. Check app_state for progress.');
      return;
    }

    operation = undefined;
    commandRunning = true;
    let reported = false;
    const stop = subscribeAsk(() => {
      if (reported || !pendingAsk()) return;
      reported = true;
      operation = { command, status: 'running' };
      agents.reply(id, true, getWindowState());
    });
    runWindowCommand(command, args).then((result) => {
      if (reported) operation = { command, status: 'completed' };
      else agents.reply(id, true, result);
    }, (error: unknown) => {
      const message = error instanceof Error ? error.message : String(error);
      if (reported) operation = { command, status: 'failed', error: message };
      else agents.reply(id, false, message);
    }).finally(() => {
      stop();
      commandRunning = false;
    });
  });
}
