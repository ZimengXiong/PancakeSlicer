import type { Config, JobLibrary, ProjectValues } from './api/types';
import { OPTIONS, jobValue } from './options';
import { filamentColor } from './plater/filamentColor';
import { meshSignature, plateDescription } from './plater/geometry';
import { projectEdits, projectInstances } from './plater/projectTransforms';
import { effectiveLayerHeight } from './profiles/context';
import { effectiveConfig, getState, presetEdits, printerPreset, resolvePreset, type LayerEvent, type Mix, type State } from './state/store';
import { slotFilaments, slotIds, slotMixes } from './profiles/slots';
import { plateSlots } from './printers/slot-check';

function eventArg(e: LayerEvent): string {
  const z = String(Math.round(e.z * 1e4) / 1e4);
  if (e.type === 'custom') return `${z}:custom:${(e.text ?? '').replace(/\r?\n/g, '\\n')}`;
  if (e.type === 'change') return `${z}:change:${e.text}`;
  return `${z}:${e.type}`;
}

export function outputKind(s = getState()): 'gcode' | 'gcode.3mf' {
  const m = printerPreset(s);
  return m?.vendor === 'BBL' ? 'gcode.3mf' : 'gcode';
}

export const MIX_KEYS = ['filament_is_mixed', 'filament_mixed_components', 'filament_mixed_sublayer_ratios', 'filament_mixed_gradient',
  'filament_mixed_gradient_range', 'filament_mixed_gradient_curve', 'filament_mixed_gradient_per_part'];

export function projectValues(s = getState()): ProjectValues {
  const own = (c: Config): Config => {
    const out: Config = {};
    for (const [k, v] of Object.entries(c)) {
      if (k === 'layer_height' || !OPTIONS[k]) continue;
      const value = jobValue(k, v);
      const values = s.project && OPTIONS[k].type === 'enum' ? OPTIONS[k].enum_values : undefined;
      if (values?.length && !values.includes(String(value))) continue;
      out[k] = value;
    }
    return out;
  };
  const process: Config = { ...s.projectPresets?.process, ...s.dirty.process };
  const filaments: Config[] = slotFilaments(s).map((key, i) => ({ ...s.projectPresets?.filaments[i], ...s.filamentEdits[key] }));
  if (s.projectMaterials && s.project) filaments.forEach((f, i) => { f.filament_colour = [filamentColor(s, i + 1)]; });
  const plate = s.projectPlates.find((p) => p.index === s.plateIndex);
  const tower = !s.project ? s.wipeTowerPos[s.plateIndex] : undefined;
  if (tower) [process.wipe_tower_x, process.wipe_tower_y] = [[tower[0]], [tower[1]]];
  if (plate && !s.project)
    for (const k of ['print_sequence', 'spiral_mode', 'first_layer_print_sequence', 'other_layers_print_sequence', 'other_layers_print_sequence_nums'])
      if (plate.settings[k] !== undefined) process[k] = plate.settings[k];
  if (Object.keys(slotMixes(s)).length) {
    const sum = (m: Mix) => m.ratios.reduce((a, b) => a + b, 0) || 100;
    filaments.forEach((f, i) => {
      const m = slotMixes(s)[i];
      Object.assign(f, { filament_is_mixed: [m ? '1' : '0'], filament_mixed_components: [m ? m.components.join(',') : ''],
        filament_mixed_sublayer_ratios: [m ? m.ratios.map((r) => Math.fround(r / sum(m)).toFixed(4)).join(',') : ''],
        filament_mixed_gradient: [m && m.gradient !== null ? '1' : '0'],
        filament_mixed_gradient_range: [!m || m.gradient === null || m.components.length !== 2 ? '' : m.gradient ? '0.1000,0.9000' : '0.9000,0.1000'],
        filament_mixed_gradient_curve: [''], filament_mixed_gradient_per_part: [m && m.gradient !== null && m.perPart ? '1' : '0'] });
    });
  }
  if (s.project && s.projectPlates.length > 1) for (const key of ['wipe_tower_x', 'wipe_tower_y']) {
    const value = process[key];
    if (Array.isArray(value) && value.length) process[key] = [Number(value[Math.min(s.plateIndex - 1, value.length - 1)])];
  }
  return { machine: own({ ...s.projectPresets?.machine, ...s.dirty.machine }), process: own(process), filaments: filaments.map(own) };
}

export function calibProject(s = getState(), kind = ''): ProjectValues {
  const own = (c: Config) => Object.fromEntries(Object.entries(c).flatMap(([k, v]) => OPTIONS[k] ? [[k, jobValue(k, v)]] : []));
  const process: Config = { ...presetEdits(s, 'process') };
  if (!kind.startsWith('nonplanar-')) process.nonplanar_mode = 'off';
  return { machine: own(presetEdits(s, 'machine')), process: own(process), filaments: [own(presetEdits(s, 'filament'))] };
}

export function jobLibrary(s: State): JobLibrary {
  const id = (type: 'machine' | 'process' | 'filament', key: string) => resolvePreset(s, type, key).record?.id ?? '';
  return { printer: id('machine', s.printer), print: id('process', s.process), filaments: slotFilaments(s).map((f) => id('filament', f)), slots: slotIds(s).map((slot) => slot ?? ''), usedSlots: plateSlots(s), layerHeight: effectiveLayerHeight(s) };
}

export function appendLibrary(fd: FormData, s: State, settings: Config = {}) {
  const library = jobLibrary(s);
  if (!library.printer) throw new Error('Pick a printer first to get started.');
  if (!library.print) throw new Error('Pick a print profile first.');
  const project = projectValues(s);
  fd.append('library', JSON.stringify({ ...library, project: { ...project, process: { ...project.process, ...settings } } }));
}

export function jobFields(s: State, saving = false) {
  return {
    library: jobLibrary(s),
    ...(s.project && s.projectPlates.length > 1 ? { plate_index: s.plateIndex } : {}),
    ...(s.project ? { project_instances: projectInstances(s) } : {}),
    ...(s.project && projectEdits(s).edits.length ? { project_edits: projectEdits(s).edits } : {}),
    project: projectValues(saving ? { ...s, projectPlates: [] } : s),
    events: s.events.map(eventArg),
    output: outputKind(s),
    ...(s.mixedTemperatures ? { allow_mixed_temperatures: true } : {}),
  };
}

export const firstLayer = (s: State) => Number(effectiveConfig(s, 'process').initial_layer_print_height) || 0.2;

export function plateSignature(s: State): string {
  return JSON.stringify({
    ...jobFields(s),
    resolved: [effectiveConfig(s, 'machine'), effectiveConfig(s, 'process'), ...slotFilaments(s).map((_, i) => effectiveConfig(s, 'filament', i))],
    plate: s.project ? undefined : plateDescription(s.objects, firstLayer(s)),
    files: s.project ? [fileId(s.project), ...projectEdits(s).added.map(meshSignature), ...projectEdits(s).volumes.map(({ o }) => meshSignature(o))] : s.objects.map(meshSignature),
  }, (key, value) => key === 'transform' && Array.isArray(value) ? value.map((v: number) => Math.round(v * 1e6) / 1e6)
    : value && typeof value === 'object' && !Array.isArray(value)
    ? Object.fromEntries(Object.entries(value).sort(([a], [b]) => a.localeCompare(b))) : value);
}

const fileId = (file: File) => `${file.name}:${file.size}:${file.lastModified}`;
