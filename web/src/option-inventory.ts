import type { ConfigValue } from './api/types';
import type { OptionDef, OptionMode } from './options';
import { normalizeConfigValue, setOptionTypes, unescapeConfigString, vectorType } from './config-values';
import { hints } from 'virtual:option-hints';


const MODES: Record<string, OptionMode> = {
  Simple: 'simple', Advanced: 'advanced', Expert: 'develop', Develop: 'developer',
  0: 'simple', 1: 'advanced', 2: 'develop', 3: 'developer',
};

interface Metadata {
  type: string;
  mode: string | number;
  enum_values?: unknown[];
  default?: unknown;
  type_id?: number;
  [key: string]: unknown;
}

function defaultValue(def: Metadata): unknown {
  const type = vectorType(def.type);
  if (type) return def.default == null ? def.default : normalizeConfigValue('', def.default as ConfigValue, type);
  if (def.type_id === undefined || typeof def.default !== 'string') return def.default;
  const value = def.default;
  const name = def.type.replace(/^co/, '').replace(/_/g, '').toLowerCase();
  if (name === 'string') return unescapeConfigString(value);
  if (value === '' && name !== 'enum') return null;
  if (name === 'bool') return value === '1' || value === 'true';
  if (name === 'float' || name === 'int' || name === 'percent') return Number(value.replace(/%$/, ''));
  if (name === 'floatorpercent') return { value: Number(value.replace(/%$/, '')), percent: value.endsWith('%') };
  if (name === 'point' || name === 'point3') return value.split(/[,x]/).map(Number);
  return value;
}

const optionType = (type: string) => type.replace(/^co/, '').replace(/Or/g, '_or_').replace(/Groups/g, '_groups').toLowerCase();

export function normalizeOptions(inventory: Record<string, Metadata>): Record<string, OptionDef> {
  return Object.fromEntries(Object.entries(inventory).map(([key, def]) => [key, {
    ...def,
    key,
    enum_values: (def.enum_values ?? []).filter((value): value is string => typeof value === 'string'),
    type: optionType(def.type),
    mode: MODES[def.mode],
    default: defaultValue(def),
  }]));
}

function records(raw: any): Record<string, Metadata> {
  const options = raw?.schema === 'orca.options.v1' && Array.isArray(raw.options)
    ? Object.fromEntries(raw.options.map((def: any) => [def.key, def])) : raw;
  if (!options || typeof options !== 'object' || !Object.keys(options).length ||
    Object.values(options).some((def: any) => !def || typeof def.type !== 'string' || MODES[def.mode] === undefined))
    throw new Error('Oops! Settings dictionary is invalid.');
  return options;
}

async function asset(url: string): Promise<any> {
  const response = await fetch(url);
  if (!response.ok) throw new Error(`${response.status} ${response.statusText}: ${url}`);
  return response.json();
}

export interface RuntimeOptions {
  options: Record<string, OptionDef>;
  etag: string | null;
  native: boolean;
  tooltips: string | null;
}

export async function loadRuntimeOptions(url: string, native: boolean, cached?: RuntimeOptions): Promise<RuntimeOptions> {
  let etag: string | null = null, tooltips: string | null = null;
  let selected: Record<string, Metadata> | null;
  if (native) {
    const response = await fetch('/api/options', {
      cache: 'no-cache',
      headers: cached?.native && cached.etag ? { 'If-None-Match': cached.etag } : {},
    });
    if (response.status === 304 && cached?.native) {
      setOptionTypes(cached.options);
      return cached;
    }
    if (!response.ok) throw new Error(`${response.status} ${response.statusText}: /api/options`);
    etag = response.headers.get('ETag');
    const body = await response.json();
    if (body?.schema !== 'orca.options.v2' || typeof body.tooltips !== 'string') throw new Error('Oops! The slicing service returned a settings format this version doesn\'t recognize.');
    tooltips = body.tooltips;
    selected = records(body.options);
  } else selected = await asset(url).then(records).catch(() => null);
  if (!selected) throw new Error('Oops! Built-in settings definitions couldn\'t be loaded.');
  const options = normalizeOptions(selected);
  if (native) for (const [key, def] of Object.entries(options)) {
    const hint = hints[key];
    if (!hint || optionType(hint.type) !== def.type) continue;
    for (const name of ['gui_type', 'multiline', 'full_width'] as const)
      if (hint[name] !== undefined) Object.assign(def, { [name]: hint[name] });
  }
  setOptionTypes(options);
  return { options, etag, native, tooltips };
}
