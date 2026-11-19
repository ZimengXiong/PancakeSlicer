import type { ProjectEdit } from '../plater/projectTransforms';

export type PresetType = 'machine' | 'process' | 'filament';
export type ConfigValue = string | number | boolean | string[] | number[];
export type Config = Record<string, ConfigValue>;

export type JobState = 'queued' | 'running' | 'done' | 'failed' | 'canceled';

export interface JobStatus {
  id: string;
  state: JobState;
  progress?: number;
  stage?: string;
  error?: string;
  error_code?: string;
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

export interface JobLibrary { printer: string; filaments: string[]; slots: string[]; print: string; layerHeight?: number;
  usedSlots?: number[] }
export interface ProjectValues { machine?: Config; process?: Config; filaments?: Config[] }
export interface JobRequest {
  library: JobLibrary;
  files: { name: string; blob: Blob }[];
  project: ProjectValues;
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
