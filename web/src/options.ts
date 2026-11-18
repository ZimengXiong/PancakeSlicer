import { brand } from './brand';
import { useEffect, useState, useSyncExternalStore } from 'react';
import { quick, removed } from 'virtual:option-bootstrap';
import optionsUrl from './options.generated.json?url&no-inline';
import processUrl from './tabs/process.json?url&no-inline';
import filamentUrl from './tabs/filament.json?url&no-inline';
import printerUrl from './tabs/printer.json?url&no-inline';
import { setOptionTypes } from './config-values';
import { normalizeOptions } from './option-inventory';
import { settingsWorker, onWorkerFailure, workerId } from './api/worker';
import type { SettingsIn, SettingsOut } from './api/settings.worker';

export type OptionMode = 'simple' | 'advanced' | 'develop' | 'developer';

export interface OptionDef {
  key: string;
  type: string;
  label?: string;
  full_label?: string;
  category?: string;
  tooltip?: string;
  sidetext?: string;
  min?: number;
  max?: number;
  max_by_mode?: Record<string, number>;
  mode: OptionMode;
  enum_values?: string[];
  enum_labels?: string[];
  enum_disabled?: string[];
  enum_notes?: (string | undefined)[];
  default?: unknown;
  readonly?: boolean;
  nullable?: boolean;
  gui_type?: string;
  multiline?: boolean;
  full_width?: boolean;
  records?: ('printer' | 'filament' | 'print')[];
}

export interface TabLine {
  options: string[];
  label?: string;
  tooltip?: string;
  modes?: Record<string, string>;
  overrides?: Record<string, Partial<OptionDef>>;
  index_labels?: string[];
  indexed_columns?: string;
}
export interface TabGroup {
  title: string;
  icon?: string;
  options: string[];
  lines: TabLine[];
  condition?: string;
}
export interface TabPage {
  title: string;
  icon: string;
  groups: TabGroup[];
  condition?: string;
}
export interface TabDesc {
  tab: 'process' | 'filament' | 'printer';
  pages: TabPage[];
  removed_keys: string[];
  visibility: { options: string[]; condition: string; effect: 'visible' | 'enabled'; when?: string }[];
  option_modes: Record<string, string>;
}

export const OPTIONS: Record<string, OptionDef> = normalizeOptions(quick);
export const TABS: Partial<Record<TabDesc['tab'], TabDesc>> = {};
const URLs = { process: processUrl, filament: filamentUrl, printer: printerUrl };
const pending = new Map<TabDesc['tab'], Promise<void>>();
const listeners = new Set<() => void>();

let tooltips: Record<string, string> | null = null, tooltipsRevision = 0;
let settleTooltips!: () => void;
const tooltipsSettled = new Promise<void>((resolve) => { settleTooltips = resolve; });
const tooltipListeners = new Set<() => void>();
function applyTooltips() {
  for (const [key, text] of Object.entries(tooltips ?? {})) if (OPTIONS[key]) OPTIONS[key].tooltip = brand(text);
}
function loadTooltips(url: string | null | undefined) {
  if (!url) { settleTooltips(); return; }
  void fetch(url).then((r) => r.ok ? r.json() as Promise<Record<string, string>> : null).then((map) => {
    if (!map) return;
    tooltips = map; applyTooltips(); tooltipsRevision++;
    for (const listener of [...tooltipListeners, ...listeners]) listener();
  }).catch(() => {}).finally(settleTooltips);
}
export const tooltipsReady = () => tooltipsSettled;
export function useTooltips() {
  return useSyncExternalStore((f) => { tooltipListeners.add(f); return () => { tooltipListeners.delete(f); }; }, () => tooltipsRevision);
}

export function loadTab(tab: TabDesc['tab']): Promise<void> {
  if (TABS[tab]) return Promise.resolve();
  const cached = pending.get(tab);
  if (cached) return cached;
  const mock = new URLSearchParams(location.search).has('mock');
  const promise = new Promise<void>((resolve, reject) => {
    const worker = settingsWorker();
    const id = workerId();
    const cleanup = () => {
      worker.removeEventListener('message', on);
      unsubscribe();
    };
    const fail = (error: Error) => { cleanup(); reject(error); };
    const on = (e: MessageEvent<SettingsOut>) => {
      if (e.data.id !== id) return;
      if (e.data.type === 'settings') {
        if (Object.keys(e.data.options).length) {
          for (const key of Object.keys(OPTIONS)) delete OPTIONS[key];
          Object.assign(OPTIONS, e.data.options);
          for (const o of Object.values(OPTIONS)) { if (o.tooltip) o.tooltip = brand(o.tooltip); if (o.label) o.label = brand(o.label); }
          applyTooltips();
          setOptionTypes(OPTIONS);
          loadTooltips(e.data.tooltips);
        }
        TABS[tab] = e.data.tab;
        cleanup();
        for (const listener of listeners) listener();
        resolve();
      } else if (e.data.type === 'error') {
        cleanup();
        reject(new Error(e.data.message));
      }
    };
    const unsubscribe = onWorkerFailure(fail);
    worker.addEventListener('message', on);
    worker.postMessage({ id, type: 'settings', native: !mock, optionsUrl: new URL(optionsUrl, location.href).href, tabUrl: new URL(URLs[tab], location.href).href } satisfies SettingsIn);
  }).catch((error) => { pending.delete(tab); throw error; });
  pending.set(tab, promise);
  return promise;
}

export function useTab(tab: TabDesc['tab']) {
  const [, update] = useState(0);
  const [error, setError] = useState<string | null>(null);
  useEffect(() => {
    let active = true;
    const changed = () => update((n) => n + 1);
    listeners.add(changed);
    setError(null);
    void loadTab(tab).then(
      () => { if (active) changed(); },
      (e) => { if (active) setError(String(e)); },
    );
    return () => { active = false; listeners.delete(changed); };
  }, [tab]);
  return { desc: TABS[tab], error };
}

export function splitKey(k: string): [string, number] {
  const [key, idx] = k.split('#');
  return [key, idx === undefined ? 0 : Number(idx)];
}

const MODE_RANK: Record<OptionMode, number> = { simple: 0, advanced: 1, develop: 2, developer: 3 };
export const modeVisible = (opt: OptionMode, ui: OptionMode) => MODE_RANK[opt] <= MODE_RANK[ui];

const HIDDEN = /^(default_)?filament_colour$|print_host|printhost|printer_agent|host_type|api_key|bbl_use_printhost|upload|cloud|device|octoprint|_plugin_config_overrides/;
const REMOVED_KEYS = new Set(removed);
const RECORD_OF = { machine: 'printer', filament: 'filament', process: 'print' } as const;
export const profileHolds = (type: 'machine' | 'filament' | 'process', key: string) => {
  const records = OPTIONS[key]?.records;
  return records ? records.includes(RECORD_OF[type]) : !!OPTIONS[key];
};
export const isHidden = (key: string) => REMOVED_KEYS.has(key) || HIDDEN.test(key);

export const DESKTOP_ONLY = new Set(['post_process', 'slicing_pipeline_plugin']);

export { toolValue, jobValue } from './config-values';
