import { strFromU8, unzipSync } from 'fflate';

const KEYS = [
  'printer_settings_id', 'print_settings_id', 'filament_settings_id', 'filament_type',
  'nozzle_diameter', 'layer_height', 'nozzle_temperature', 'nozzle_temperature_initial_layer',
  'curr_bed_type', 'textured_plate_temp', 'textured_plate_temp_initial_layer', 'hot_plate_temp',
  'cool_plate_temp', 'eng_plate_temp', 'filament_max_volumetric_speed', 'fan_max_speed',
  'filament_retraction_length', 'enable_support', 'support_type', 'wall_loops', 'sparse_infill_density',
  'sparse_infill_pattern', 'brim_type', 'brim_width', 'top_shell_layers', 'bottom_shell_layers',
];

export interface GcodeReport {
  plate?: string;
  settings: Record<string, string>;
  nozzle_temperatures_set: number[];
  bed_temperatures_set: number[];
  header: Record<string, string>;
  slice_info?: {
    plates: Record<string, string>[];
    filaments: Record<string, string>[];
    warnings: Record<string, string>[];
    objects: Record<string, string>[];
  };
}

function attributes(tag: string) {
  const out: Record<string, string> = {};
  for (const m of tag.matchAll(/([\w:-]+)="([^"]*)"/g)) out[m[1]] = m[2];
  return out;
}

function sliceInfo(xml: string): GcodeReport['slice_info'] {
  const plates: Record<string, string>[] = [];
  for (const block of xml.matchAll(/<plate>([\s\S]*?)<\/plate>/g)) {
    const meta: Record<string, string> = {};
    for (const m of block[1].matchAll(/<metadata\s+key="([^"]+)"\s+value="([^"]*)"\s*\/>/g)) meta[m[1]] = m[2];
    plates.push(meta);
  }
  const pick = (tag: string) => [...xml.matchAll(new RegExp(`<${tag}\\s[^>]*>`, 'g'))].map((m) => attributes(m[0]));
  return { plates, filaments: pick('filament'), warnings: pick('warning'), objects: pick('object') };
}

function gcodeReport(gcode: string): Omit<GcodeReport, 'slice_info' | 'plate'> {
  const settings: Record<string, string> = {};
  for (const key of KEYS) {
    const m = new RegExp(`^; ${key} = (.*)$`, 'm').exec(gcode);
    if (m) settings[key] = m[1].trim();
  }
  const header: Record<string, string> = {};
  const head = /; HEADER_BLOCK_START([\s\S]*?); HEADER_BLOCK_END/.exec(gcode);
  if (head) {
    for (const line of head[1].split('\n')) {
      const m = /^;\s*([^:]+?):\s(.*)$/.exec(line.trim());
      if (m) header[m[1].trim()] = m[2].trim();
    }
  }
  const temps = (re: RegExp) => [...new Set([...gcode.matchAll(re)].map((m) => Number(m[1])))].sort((a, b) => a - b);
  return { settings, header, nozzle_temperatures_set: temps(/^M10[49] S(\d+)/gm), bed_temperatures_set: temps(/^M1[49]0 S(\d+)/gm) };
}

export function inspectOutput(data: Uint8Array, filename: string): GcodeReport {
  if (!/\.3mf$/i.test(filename)) return gcodeReport(strFromU8(data));
  const files = unzipSync(data, { filter: (f) => /^Metadata\/(plate_\d+\.gcode|slice_info\.config)$/.test(f.name) });
  const plate = Object.keys(files).filter((n) => n.endsWith('.gcode')).sort()[0];
  if (!plate) throw new Error(`${filename} has no G-code plate inside.`);
  const report: GcodeReport = { plate, ...gcodeReport(strFromU8(files[plate])) };
  const info = files['Metadata/slice_info.config'];
  if (info) report.slice_info = sliceInfo(strFromU8(info));
  return report;
}
