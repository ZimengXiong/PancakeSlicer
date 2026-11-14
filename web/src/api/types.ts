import type { ProfileBaseAnchor, ProfileOwnership } from '../profiles/ownership';
import type { ProjectEdit } from '../plater/projectTransforms';

export type PresetType = 'machine' | 'process' | 'filament';
export type ConfigValue = string | number | boolean | string[] | number[];
export type Config = Record<string, ConfigValue>;

export interface SystemPreset {
  name: string;
  version?: string;
  imported?: boolean;
  engine_base?: string;
  setting_id?: string;
  base_id?: string;
  inherits?: string[];
  native_support_capable?: boolean;
  vendor: string;
  type: PresetType;
  compatible_printers?: string[];
  compatible_evaluated?: boolean;
  compatible_prints?: string[];
  filament_id?: string;
  config: Config;
  model?: string;
  variant?: string;
  bed_model?: string;
  bed_texture?: string;
  default_bed_type?: string;
  not_support_bed_type?: string[];
  default_materials?: string[];
}

export interface ProfileOverlay extends ProfileOwnership {
  id?: string;
  aliases?: string[];
  name: string;
  type: PresetType;
  base: ProfileBaseAnchor;
  parent?: string;
  overlay: Config;
  baseline?: Record<string, ConfigValue | null>;
  metadata?: Config;
}

export interface UserPreset extends ProfileOwnership {
  id?: string;
  aliases?: string[];
  name: string;
  type: PresetType;
  inherits: string;
  overrides: Config;
  base?: ProfileOverlay['base'];
  parent?: string;
  overlay?: Config;
  baseline?: ProfileOverlay['baseline'];
  metadata?: Config;
}

export interface PresetCatalog {
  machines: SystemPreset[];
  processes: SystemPreset[];
  filaments: SystemPreset[];
}

export type JobState = 'idle' | 'queued' | 'preparing' | 'slicing' | 'storing' | 'done' | 'failed' | 'canceling' | 'cancelled' | 'interrupted' | 'stale';

export interface JobStatus {
  id: string;
  state: JobState;
  progress?: number;
  stage?: string;
  error?: string;
  record?: Record<string, unknown>;
  has_surface_grid?: boolean;
  output?: 'gcode' | 'gcode.3mf' | '3mf';
  filename?: string;
  timeout_warning?: boolean;
}

export interface ProgressEvent {
  stage: string;
  pct: number;
  message?: string;
  queuePosition?: number;
}

export interface PlateJson {
  objects: {
    file: string;
    name: string;
    extruder?: number;
    instances: { x: number; y: number; rot: number; z?: number; printable?: boolean }[];
    plate?: number;
    paint?: Record<string, Record<string, string>>;
    brim_ears?: { x: number; y: number; z: number; radius: number }[];
    layer_height_profile?: number[];
    ranges?: { z: [number, number]; layer_height: number; settings: Record<string, string> }[];
    settings?: Record<string, string>;
    volumes?: { file: string; type: string; name: string; extruder?: number; settings?: Record<string, string>; paint?: Record<string, Record<string, string>> }[];
  }[];
  plates?: { index: number; name: string; settings: Record<string, string>; locked?: boolean }[];
}

export interface JobLibrary { printer: string; filaments: string[]; print: string; layerHeight?: number }
export interface JobRequest {
  library: JobLibrary;
  files: { name: string; blob: Blob }[];
  overrides: Config;
  plate?: PlateJson;
  plate_index?: number;
  project_instances?: { printable: boolean; transform?: number[] }[];
  project_edits?: ProjectEdit[];
  events: string[];
  output: 'gcode' | 'gcode.3mf' | '3mf';
  project_settings?: Config;
  calib?: string;
  calib_params?: string;
  calib_head?: string;
  force_planar?: boolean;
  floating?: boolean;
  allow_mixed_temperatures?: boolean;
  priority?: 'low';
  preview_budget?: number;
  plan_tops?: boolean;
}
