import { confirmDialog, messageDialog } from './components/Ask';
import { undoable } from './state/history';
import { effectiveConfig, getState, markStale, presetEdits, setState } from './state/store';
import type { Config, ConfigValue, PresetType } from './api/types';
import { slotFilaments } from './profiles/slots';

const TITLE = 'Warning';
const first = (v: ConfigValue | undefined) => Number(String(Array.isArray(v) ? v[0] : v ?? '').split(',')[0]);
const bool = (v: ConfigValue | undefined) => v === true || v === '1' || v === 'true' || v === 1;

function setOptions(type: PresetType, patch: Config, name: string, slot = 0) {
  undoable(name, () => setState((s) => {
    const edits = { ...presetEdits(s, type, slot), ...patch };
    return {
      ...(type === 'filament' ? { filamentEdits: { ...s.filamentEdits, [slotFilaments(s)[slot]]: edits } } : { dirty: { ...s.dirty, [type]: edits } }),
      job: markStale(s.job),
    };
  }));
}

const SPIRAL: Config = { wall_loops: '1', top_shell_layers: '0', sparse_infill_density: '0%', enable_support: '0', enforce_support_layers: '0',
  detect_thin_wall: '0', overhang_reverse: '0', timelapse_type: '0', enable_wrapping_detection: '0' };
export const SPIRAL_KEYS = ['spiral_mode', ...Object.keys(SPIRAL)];

async function checkSpiral() {
  const s = getState(), c = effectiveConfig(s, 'process');
  if (!bool(c.spiral_mode)) return;
  const ok = first(c.wall_loops) === 1 && first(c.top_shell_layers) === 0 && parseFloat(String(c.sparse_infill_density)) === 0 && !bool(c.enable_support)
    && first(c.enforce_support_layers) === 0 && !bool(c.detect_thin_wall) && !bool(c.overhang_reverse) && String(c.timelapse_type ?? '0') === '0'
    && !bool(c.enable_wrapping_detection);
  if (ok) return;
  let msg = 'Spiral Vase mode prints a single continuous outer wall—it sets wall loops to 1, top shell layers to 0, infill to 0%, turns off supports and clumping detection, and sets timelapse to standard.';
  if (effectiveConfig(s, 'machine').printer_structure === 'i3') msg += ' But machines with I3 structure will not generate timelapse videos.';
  msg += '\n\nChange these settings automatically?\nYes - Change these settings and enable spiral/vase mode automatically\nNo  - Cancel enabling spiral mode';
  if (await confirmDialog(TITLE, msg, ['Yes', 'No'])) setOptions('process', SPIRAL, 'Enable spiral vase');
  else setOptions('process', { spiral_mode: '0' }, 'Cancel spiral vase');
}

async function checkLayerHeight() {
  const s = getState(), m = effectiveConfig(s, 'machine');
  const list = (v: ConfigValue | undefined) => String(Array.isArray(v) ? v.join(',') : v ?? '').split(',').map(Number).filter(Number.isFinite);
  const mins = list(m.min_layer_height), maxs = list(m.max_layer_height);
  const min = mins.length ? Math.min(...mins) : 0, max = maxs.length ? Math.max(...maxs) : 0;
  const h = Number(effectiveConfig(s, 'process').layer_height), EPS = 1e-4;
  if (min > EPS && !(h >= EPS)) {
    await messageDialog(TITLE, `That layer height is too thin—we bumped it up to the minimum (${min} mm).`);
    return setOptions('process', { layer_height: String(min) }, 'Change setting "Layer height"');
  }
  const to = max > EPS && h > max + EPS ? max : min > EPS && h < min - EPS ? min : null;
  if (to === null) return;
  const msg = 'Layer height is outside the limits set in Printer Settings -> Extruder -> Layer height limits, this may cause printing quality issues.'
    + `\n\nAdjust it to the limit (${to} mm) automatically?`;
  if (await confirmDialog(TITLE, msg, ['Adjust', 'Ignore'])) setOptions('process', { layer_height: String(to) }, 'Change setting "Layer height"');
}

async function checkNozzleTemperature(key: string, slot: number) {
  const c = effectiveConfig(getState(), 'filament', slot);
  const low = first(c.nozzle_temperature_range_low), high = first(c.nozzle_temperature_range_high), t = first(c[key]);
  if (!Number.isFinite(low) || !Number.isFinite(high) || !Number.isFinite(t) || (t >= low && t <= high)) return;
  await messageDialog(TITLE, 'Heads up: printing outside the recommended temperature range can cause nozzle clogs or poor layer bonding.\nAre you sure you want to use this temperature?\n\n'
    + `The recommended nozzle temperature for this filament is ${low}–${high} °C.`);
}

const MATERIAL_RANGE = new Map('ABS:190-300 ABS-CF:220-300 ABS-GF:240-280 ASA:220-300 ASA-CF:230-300 ASA-GF:240-300 ASA-AERO:240-280 BVOH:190-240 EVA:175-220 FLEX:210-230 HIPS:220-270 PA:235-280 PA-CF:240-315 PA-GF:240-290 PA6:260-300 PA6-CF:230-300 PA6-GF:260-300 PA11:275-295 PA11-CF:275-295 PA11-GF:275-295 PA12:250-270 PA12-CF:250-300 PA12-GF:255-270 PAHT:260-310 PAHT-CF:270-310 PAHT-GF:270-310 PC:240-300 PC-ABS:230-270 PC-CF:270-295 PC-PBT:260-300 PCL:130-170 PCTG:220-300 PE:175-260 PE-CF:175-260 PE-GF:230-270 PEI-1010:370-430 PEI-1010-CF:380-430 PEI-1010-GF:380-430 PEI-9085:350-390 PEI-9085-CF:365-390 PEI-9085-GF:370-390 PEEK:350-460 PEEK-CF:380-410 PEEK-GF:375-410 PEKK:325-400 PEKK-CF:360-400 PES:340-390 PET:200-290 PET-CF:240-320 PET-GF:280-320 PETG:190-260 PETG-CF:230-290 PETG-GF:210-270 PHA:190-250 PI:390-410 PLA:180-240 PLA-AERO:220-270 PLA-CF:190-250 POM:210-250 PP:200-240 PP-CF:210-250 PP-GF:220-260 PPA-CF:260-300 PPA-GF:260-290 PPS:300-345 PPS-CF:295-350 PPSU:360-420 PSU:350-380 PVA:185-250 PVB:190-250 PVDF:245-265 SBS:195-250 TPI:420-445 TPU:175-260'.split(' ').map((e) => { const [t, r] = e.split(':'); return [t, r.split('-').map(Number)] as const; }));

async function checkRecommendedRange(slot: number) {
  const c = effectiveConfig(getState(), 'filament', slot);
  const low = first(c.nozzle_temperature_range_low), high = first(c.nozzle_temperature_range_high);
  if (!Number.isFinite(low) || !Number.isFinite(high)) return;
  let type = String(Array.isArray(c.filament_type) ? c.filament_type[0] : c.filament_type ?? '');
  const [min, max] = MATERIAL_RANGE.get(type) ?? [190, 300];
  if (!MATERIAL_RANGE.has(type)) type = 'Unknown';
  let msg = '';
  if (low < min) msg += `We recommend a minimum temperature above ${min} °C for ${type}.
`;
  if (high > max) msg += `We recommend a maximum temperature below ${max} °C for ${type}.
`;
  if (low > high) msg += 'The minimum temperature can\'t be higher than the maximum temperature.\n';
  if (msg) await messageDialog(TITLE, msg + 'Please double-check this setting.\n');
}

async function checkAlternateExtraWall() {
  const c = effectiveConfig(getState(), 'process');
  if (!bool(c.alternate_extra_wall) || String(c.ensure_vertical_shell_thickness) !== 'ensure_all') return;
  const msg = 'Alternate extra wall works best when \'Ensure vertical shell thickness\' is set to Moderate instead of All.\n\nWould you like to update both automatically?\n'
    + 'Yes — switch vertical shell thickness to Moderate and turn on Alternate extra wall\nNo — keep current settings';
  if (await confirmDialog(TITLE, msg, ['Yes', 'No'])) setOptions('process', { ensure_vertical_shell_thickness: 'ensure_moderate', alternate_extra_wall: '1' }, 'Change setting "Alternate extra wall"');
  else setOptions('process', { ensure_vertical_shell_thickness: 'ensure_all', alternate_extra_wall: '0' }, 'Change setting "Alternate extra wall"');
}

async function checkSeamSlope() {
  const c = effectiveConfig(getState(), 'process');
  const h = Number(c.layer_height), v = String(c.seam_slope_start_height ?? '0');
  const start = v.endsWith('%') ? (parseFloat(v) / 100) * h : parseFloat(v);
  if (String(c.seam_slope_type ?? 'none') === 'none' || !(start >= h)) return;
  await messageDialog(TITLE, 'seam_slope_start_height needs to be smaller than layer_height.\nReset to 0.');
  setOptions('process', { seam_slope_start_height: '0' }, 'Change setting "Scarf start height"');
}

async function checkVolumetricSpeed(slot: number) {
  if (!(first(effectiveConfig(getState(), 'filament', slot).filament_max_volumetric_speed) < 0.5)) return;
  await messageDialog(TITLE, 'Max volumetric speed was too low, so we reset it to 0.5 mm³/s.');
  setOptions('filament', { filament_max_volumetric_speed: '0.5' }, 'Change setting "Max volumetric speed"', slot);
}

export async function checkSetting(type: PresetType, key: string, slot = 0) {
  if (type === 'process' && SPIRAL_KEYS.includes(key)) return checkSpiral();
  if (type === 'process' && key === 'layer_height') { await checkLayerHeight(); return checkSeamSlope(); }
  if (type === 'process' && (key === 'alternate_extra_wall' || key === 'ensure_vertical_shell_thickness')) return checkAlternateExtraWall();
  if (type === 'process' && (key === 'seam_slope_type' || key === 'seam_slope_start_height')) return checkSeamSlope();
  if (type === 'filament' && (key === 'nozzle_temperature_range_low' || key === 'nozzle_temperature_range_high')) return checkRecommendedRange(slot);
  if (type === 'filament' && key === 'filament_max_volumetric_speed') return checkVolumetricSpeed(slot);
  if (type === 'filament' && (key === 'nozzle_temperature' || key === 'nozzle_temperature_initial_layer')) return checkNozzleTemperature(key, slot);
}
