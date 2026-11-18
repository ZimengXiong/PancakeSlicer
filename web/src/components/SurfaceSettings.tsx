import { useEffect, type ReactNode } from 'react';
import { openSlideOutEditor } from '../navigation';
import { probeEngine, useEngineHealth } from '../flow/engine-health';
import type { Config, ConfigValue } from '../api/types';
import { OPTIONS, modeVisible, type OptionDef, type OptionMode } from '../options';
import { methodSelectionProblem, type MethodAvailability, type MethodAvailabilityResult } from '../profiles/capabilities';
import { getState, resetOption, setOption, setState, useStore } from '../state/store';
import { openSlantPlane, slantTarget } from '../plater/slant';
import { toolArtifactManifest } from '../api/client';
import type { ProfileFields } from './SettingsTab';
import { Field } from './Field';
import { GroupHead, isGroupOpen, markOf, setGroupOpen, useSettingsGroups } from './SettingsGroup';
import './process-settings.css';

const shortReason = (m: MethodAvailability) => m.recovery === 'update_engine' ? (toolArtifactManifest ? 'Not in this engine build' : 'Engine still starting')
  : m.recovery === 'repair_printer' ? 'Printer axes unknown'
  : /rotary/i.test(m.reason ?? '') ? 'Needs rotary axes'
  : /Auto/.test(m.reason ?? '') ? 'Experimental on this printer' : 'Not available';
const openMethodChoice = (key: string) => {
  const trigger = document.querySelector<HTMLElement>(`.opt-line[data-opt="${key}"] .dropdown`);
  trigger?.focus(); trigger?.click();
};
import { MODE_LABELS, MODE_HELP as BASE_MODE_HELP } from '../nonplanar-mode';
const OFFERED_MODES = ['off', 'nonplanar_top', 'smooth_morph', 'slant', 'conformal_layers', 'auto'], ROTARY_MODES = ['multiplanar', 'field'];
const MODE_HELP: Record<string, string> = {
  ...BASE_MODE_HELP,
  multiplanar: 'Multiplanar: splits the model into tilted flat-layer sections so overhangs need fewer supports.',
  field: 'Rotary curved layers: prints the entire part in curved layers while the rotary table tilts to keep every pass level under the nozzle.',
};
export function methodModes(current: string, availability: MethodAvailabilityResult): {enum_values: string[]; enum_labels: string[]; tooltip: string} {
  const offered = [...OFFERED_MODES, ...availability.printer.rotary ? ROTARY_MODES : []];
  const values = offered.includes(current) ? offered : [current, ...offered];
  const def = OPTIONS.nonplanar_mode;
  return {enum_values: values, enum_labels: values.map(v => MODE_LABELS[v] ?? def?.enum_labels?.[def.enum_values?.indexOf(v) ?? -1] ?? v),
    tooltip: offered.map(v => MODE_HELP[v]).join('\n')};
}
const TOP_LAYERS = 'nonplanar_curved_top_layers', TOP_DIRECTION = 'nonplanar_top_direction', SLANT_DIRECTION = 'nonplanar_slant_direction';
const MORPH = 'nonplanar_slant_morph_layers', WAVE = 'nonplanar_wave', ZAA = 'zaa_enabled', PLANE = 'slant_plane';
const WAVE_ROWS = ['nonplanar_wave_pattern', 'nonplanar_wave_amplitude', 'nonplanar_wave_wavelength', 'nonplanar_wave_direction'];
const ZAA_ROWS = ['zaa_minimize_perimeter_height', 'zaa_min_z', 'zaa_dont_alternate_fill_direction', 'zaa_mid_layer_slice'];
const CLEARANCE = ['nonplanar_ignore_duct', 'nonplanar_collision_tolerance', 'nonplanar_duct_graze_limit', 'nonplanar_curved_max_angle',
  'nonplanar_collision_check', 'nonplanar_repair_iterations'];
const ROTARY_AUTO_TUNING = ['nonplanar_full_conformal_max_thickness', 'nonplanar_steep_skin', 'nonplanar_spiral_top', 'nonplanar_tilt_flow',
  'nonplanar_top_finish_flow', 'nonplanar_top_order', 'nonplanar_top_speed_factor', 'nonplanar_top_fan_boost', 'nonplanar_road_cooling',
  'nonplanar_flow_mod', 'nonplanar_surface_spacing', 'nonplanar_strength_roads', 'nonplanar_surface_resolution', 'nonplanar_shell_layer_height',
  'nonplanar_max_patch_height', 'nonplanar_adaptive_bands', 'nonplanar_ironing'];
interface MethodRows { main: string[]; tuning: string[]; clearance: string[] }
const METHOD_ROWS: Record<string, MethodRows> = {
  off: { main: [WAVE, ZAA, 'nonplanar_sublayer_walls'], tuning: [], clearance: [] },
  nonplanar_top: { main: [TOP_LAYERS, TOP_DIRECTION, 'nonplanar_sublayer_walls'],
    tuning: ['nonplanar_top_finish_flow', 'nonplanar_top_order', 'nonplanar_curved_shell_flow', 'nonplanar_curved_shell_speed', 'nonplanar_curved_min_area'],
    clearance: CLEARANCE.slice(0, 4) },
  smooth_morph: { main: [TOP_LAYERS, 'nonplanar_morph_layers', TOP_DIRECTION],
    tuning: ['nonplanar_top_finish_flow', 'nonplanar_top_order', 'nonplanar_top_speed_factor', 'nonplanar_top_fan_boost', 'nonplanar_road_cooling',
      'nonplanar_flow_mod', 'nonplanar_max_patch_height'], clearance: CLEARANCE },
  conformal_layers: { main: [TOP_LAYERS, TOP_DIRECTION],
    tuning: ['nonplanar_conformal_ramp_layers', 'nonplanar_top_finish_flow', 'nonplanar_top_order', 'nonplanar_curved_min_area', 'nonplanar_conformal_rounding',
      'nonplanar_conformal_fallback_angle', 'nonplanar_road_cooling', 'nonplanar_flow_mod'], clearance: CLEARANCE },
  slant: { main: ['nonplanar_slant_angle', SLANT_DIRECTION, PLANE, MORPH, 'nonplanar_slant_start_height'],
    tuning: ['nonplanar_top_finish_flow', TOP_DIRECTION, 'nonplanar_top_order', 'nonplanar_road_cooling', 'nonplanar_flow_mod'],
    clearance: CLEARANCE.filter(key => key !== 'nonplanar_curved_max_angle') },
  auto: { main: [TOP_LAYERS, 'nonplanar_morph_layers', TOP_DIRECTION, 'nonplanar_sublayer_walls'],
    tuning: ['nonplanar_top_finish_flow', 'nonplanar_top_order', 'nonplanar_curved_shell_flow', 'nonplanar_curved_shell_speed', 'nonplanar_curved_min_area',
      'nonplanar_top_speed_factor', 'nonplanar_top_fan_boost', 'nonplanar_road_cooling', 'nonplanar_flow_mod', 'nonplanar_max_patch_height'], clearance: CLEARANCE },
};
const ROTARY_AUTO: MethodRows = { main: ['nonplanar_deform', 'nonplanar_sublayer_walls', ZAA], tuning: ROTARY_AUTO_TUNING, clearance: CLEARANCE };
const ROTARY_METHOD: MethodRows = { main: ['multiplanar_max_planes', 'multiplanar_overhang_threshold'], tuning: [], clearance: CLEARANCE };
const methodRows = (mode: string, rotary: boolean): MethodRows => mode === 'auto' && rotary ? ROTARY_AUTO
  : METHOD_ROWS[mode] ?? (['multiplanar', 'field', 'oneshot'].includes(mode) ? ROTARY_METHOD : { main: [], tuning: [], clearance: CLEARANCE });
const SUB_ROWS: Record<string, string[]> = { [WAVE]: WAVE_ROWS, [ZAA]: ZAA_ROWS };
export const TOP_METHOD_KEYS = new Set(['nonplanar_mode', ...[...Object.values(METHOD_ROWS), ROTARY_AUTO, ROTARY_METHOD].flatMap(r => [...r.main, ...r.tuning, ...r.clearance]),
  ...WAVE_ROWS, ...ZAA_ROWS]);
export const RETIRED_GROUPS = new Set(['Non-planar', 'Curved top finish', 'Non-planar tuning', 'Z contouring']);
const EXPERIMENTAL = new Set(['slant', 'conformal_layers']);
const groupId = (title: string) => `process/Quality/${title}`;
const CLEARANCE_GROUP = 'Head clearance', TUNING_GROUP = 'Advanced non-planar';

export function SurfaceSettings({config,system,availability,editor,unsaved,uiMode,legacyKeys = [],readOnly = false}: {config:Config;system:Config;availability:MethodAvailabilityResult;editor?:ProfileFields;unsaved:(key:string)=>boolean;uiMode:OptionMode;legacyKeys?:string[];readOnly?:boolean}) {
  const engine = useEngineHealth();
  useEffect(() => {void probeEngine();}, []);
  const mode = String(config.nonplanar_mode ?? '');
  const slantObject = useStore((s) => !!slantTarget(s));
  const lone = useStore((s) => s.objects.length === 1 ? s.objects[0] : null);
  const focusKey = useStore((s) => s.settingsFocus?.type === 'process' ? s.settingsFocus.key : null);
  const wave = String(config[WAVE] ?? '');
  const problem = methodSelectionProblem(availability,config);
  const value = (key: string) => config[key];
  const on = (key: string) => ['1', 'true'].includes(String(value(key)));
  const write = (key: string, next: string) => editor ? editor.set(key, next) : next === String(system[key] ?? '') ? resetOption('process', key) : setOption('process', key, next);
  const row = (key: string, extra: {label?: string; value?: string; definition?: Partial<OptionDef>; onSet?: (key: string, value: ConfigValue) => void; rowKey?: string} = {}) =>
    OPTIONS[key] ? <Field key={extra.rowKey ?? key} type="process" optKey={key} label={extra.label} value={extra.value ?? (key === 'nonplanar_mode' ? mode : config[key])} systemValue={system[key]}
      dirty={unsaved(key)} onSet={extra.onSet ?? editor?.set} onReset={editor?.reset} disabled={readOnly} mode={mode} definition={extra.definition}/> : null;
  const selector = (key: string, target: 'mode' | 'wave') => {
    const def = OPTIONS[key]; if (!def) return null;
    const current = key === 'nonplanar_mode' ? mode : String(config[key] ?? '');
    const methods = availability.methods.filter(m => m.target === target);
    const modes = target === 'mode' ? methodModes(current, availability) : undefined;
    const values = modes?.enum_values ?? [...new Set(['off', ...methods.map(m => m.id), current])];
    const labels = modes?.enum_labels ?? values.map(v => def.enum_labels?.[def.enum_values?.indexOf(v) ?? -1] ?? v);
    const blocked = values.map(v => methods.find(m => m.id === v && !m.supported));
    return row(key, {label: target === 'mode' ? 'Slicing method' : undefined, definition: {...modes?.tooltip && {tooltip: modes.tooltip}, gui_type: 'select_close', enum_values: values,
      enum_labels: labels, enum_disabled: values.filter((_, i) => blocked[i]), enum_notes: blocked.map((m, i) => m && values[i] !== current ? shortReason(m) : undefined)}});
  };
  const autoOrValue = (key: string, named: [string, string][], isNamed: (n: number) => string | undefined, custom: string, valueLabel: string) => {
    const def = OPTIONS[key]; if (!def) return [];
    const n = Number(value(key)), name = isNamed(n), customValue = name === undefined ? String(value(key)) : custom;
    const choice = row(key, {value: name ?? customValue, definition: {gui_type: 'select_close', enum_values: [...named.map(([v]) => v), customValue], enum_labels: [...named.map(([, l]) => l), 'Custom']}});
    return name === undefined ? [choice, row(key, {label: valueLabel, rowKey: `${key}-value`})] : [choice];
  };
  const shells = Math.max(Number(config.top_shell_layers ?? 0) || 0, Math.ceil((Number(config.top_shell_thickness ?? 0) || 0) / (Number(config.layer_height ?? 0.2) || 0.2) - 1e-6));
  const topLayersAuto = mode === 'nonplanar_top' ? 'Auto (3)' : mode === 'auto' ? 'Auto' : `Auto (${Math.max(1, shells)})`;
  const special: Record<string, () => ReactNode[]> = {
    [WAVE]: () => [selector(WAVE, 'wave'), ...wave !== 'off' ? WAVE_ROWS.map(sub => row(sub)) : []],
    [TOP_LAYERS]: () => autoOrValue(TOP_LAYERS, [['0', topLayersAuto]], n => n > 0 ? undefined : '0', mode === 'nonplanar_top' ? '3' : String(Math.max(1, shells)), 'Layers'),
    [TOP_DIRECTION]: () => autoOrValue(TOP_DIRECTION, [['-2', 'Auto'], ['-1', 'Follow level lines']], n => n <= -2 ? '-2' : n < 0 ? '-1' : undefined, '45', 'Angle'),
    [SLANT_DIRECTION]: () => autoOrValue(SLANT_DIRECTION, [['-1', 'Auto']], n => n < 0 ? '-1' : undefined, '0', 'Angle'),
    [MORPH]: () => {
      const layers = Number(value(MORPH)) || 0;
      const toggle = row(MORPH, {label: 'Morphing', rowKey: `${MORPH}-on`, value: layers > 0 ? '1' : '0', definition: {type: 'bool', tooltip: "Off: transitions sharply at the slant plane (flat layers below, tilted layers above). On: smoothly bends layers from flat into the slant angle."},
        onSet: (_, v) => write(MORPH, ['1', 'true'].includes(String(v)) ? '10' : '0')});
      return layers > 0 ? [toggle, row(MORPH)] : [toggle];
    },
    [PLANE]: () => [<div key={PLANE} className="opt-line">
      <span className="opt-label"><span className="opt-label-text">Slant plane</span></span>
      <fieldset className="opt-field" disabled={readOnly || (!slantObject && !lone)}>
        <button type="button" className="btn" title={slantObject || lone ? undefined : 'Select a model on the plate to set its slant plane'}
          onClick={() => { if (!slantObject && lone) setState({selected: {object: lone.id, instance: lone.instances[0].id}}); openSlantPlane(); }}>Define</button>
      </fieldset>
    </div>],
  };
  const render = (key: string) => special[key]?.() ?? [row(key), ...key === ZAA && on(ZAA) ? ZAA_ROWS.map(sub => row(sub)) : []];
  const notice = (target:'mode'|'wave') => {
    if (!problem || (target === 'mode' ? mode : wave) !== problem.method) return null;
    const method = availability.methods.find(m => m.target === target && m.id === problem.method);
    const key = target === 'mode' ? 'nonplanar_mode' : WAVE;
    return <div key={`${key}-why`} className="surface-why" role="alert"><span>{method ? shortReason(method) : 'Not available'}</span>
      {problem.recovery === 'repair_printer'
        ? <button type="button" className="link" onClick={() => openSlideOutEditor('machine', getState().printer)}>Edit printer axes</button>
        : <button type="button" className="link" onClick={() => openMethodChoice(key)}>Choose another method</button>}
    </div>;
  };
  useSettingsGroups();
  const ready = mode === 'off' || availability.allowedModes.includes(mode);
  const rows = methodRows(mode, availability.printer.rotary);
  const shown = (key: string) => key === PLANE || !!OPTIONS[key] && (modeVisible(OPTIONS[key].mode, uiMode) || OPTIONS[key].mode !== 'develop');
  const tuned = (key: string) => !!OPTIONS[key] && modeVisible(OPTIONS[key].mode, uiMode);
  const overriding = mode === 'off' ? [] : [...(wave !== 'off' ? [WAVE] : []), ...(on(ZAA) && !rows.main.includes(ZAA) ? [ZAA] : [])];
  const main = ready ? [...rows.main.filter(key => shown(key) && (key !== WAVE || availability.allowedWaveModes.length > 0 || wave !== 'off')), ...overriding] : [];
  const tuning = ready ? [...rows.tuning, ...(METHOD_ROWS[mode] ? [] : legacyKeys).filter(key => !rows.main.includes(key) && !rows.clearance.includes(key) && key !== 'nonplanar_mode'),
    ...mode === 'off' && wave !== 'off' ? ['nonplanar_top_finish_flow', 'nonplanar_road_cooling', 'nonplanar_flow_mod'] : []].filter(tuned) : [];
  const clearance = ready && (mode !== 'off' || wave !== 'off') ? (mode === 'off' ? CLEARANCE.slice(0, 3).concat('nonplanar_collision_check') : rows.clearance).filter(shown) : [];
  const title = MODE_LABELS[mode] ?? OPTIONS.nonplanar_mode?.enum_labels?.[OPTIONS.nonplanar_mode.enum_values?.indexOf(mode) ?? -1] ?? mode;
  useEffect(() => {
    if (!focusKey) return;
    if (main.includes(focusKey) || Object.entries(SUB_ROWS).some(([k, subs]) => main.includes(k) && subs.includes(focusKey))) setGroupOpen(groupId(title), true);
    else if (clearance.includes(focusKey)) setGroupOpen(groupId(CLEARANCE_GROUP), true);
    else if (tuning.includes(focusKey)) setGroupOpen(groupId(TUNING_GROUP), true);
  }, [focusKey]);
  const group = (name: string, keys: string[], byDefault: boolean, tag?: string) => {
    if (!keys.length) return null;
    const open = isGroupOpen(groupId(name), byDefault);
    return <section className="optgroup" data-collapsed={open ? undefined : ''}>
      <GroupHead id={groupId(name)} title={name} tag={tag} open={open} modified={markOf(keys.flatMap((key) => [key, ...SUB_ROWS[key] ?? []]), unsaved)}/>
      {open && keys.flatMap(render)}
    </section>;
  };
  return <>
  <section className="optgroup surface-settings" data-engine-state={engine.value}>
    {selector('nonplanar_mode', 'mode')}
    {notice('mode')}
    {notice('wave')}
  </section>
  {group(title, main, true, EXPERIMENTAL.has(mode) ? 'Experimental' : undefined)}
  {group(CLEARANCE_GROUP, clearance, false)}
  {group(TUNING_GROUP, tuning, false)}
  </>;
}
