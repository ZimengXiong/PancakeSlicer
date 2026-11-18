import { useEffect, useState } from 'react';
import type { Config, ConfigValue, PresetType } from '../api/types';
import { normalizeConfigValue, toolValue } from '../config-values';
import { loadTab, OPTIONS, splitKey, TABS, isHidden } from '../options';
import { display } from '../profiles/format';

export { display };

const TAB = { machine: 'printer', filament: 'filament', process: 'process' } as const;

export interface DiffRow { key: string; label: string; group: string; values: (ConfigValue | undefined)[]; differs: boolean }

export function diffRows(type: PresetType, configs: Config[], all = false): DiffRow[] {
  const rows: DiffRow[] = [];
  const seen = new Set<string>();
  const differs = (k: string) => new Set(configs.map((c) => {
    const v = c[k];
    return v === undefined ? '' : toolValue(k, normalizeConfigValue(k, v));
  })).size > 1;
  const values = (k: string) => configs.map((c) => c[k]);
  for (const page of TABS[TAB[type]]?.pages ?? []) for (const group of page.groups) for (const line of group.lines) for (const k of line.options) {
    const key = splitKey(k)[0];
    if (seen.has(key) || !OPTIONS[key] || isHidden(key)) continue;
    seen.add(key);
    const d = differs(key);
    if (d || all) rows.push({ key, label: OPTIONS[key].label || line.label || key, group: `${page.title} › ${group.title}`, values: values(key), differs: d });
  }
  for (const key of new Set(configs.flatMap((c) => Object.keys(c))))
    if (!seen.has(key) && OPTIONS[key] && !isHidden(key) && differs(key))
      rows.push({ key, label: OPTIONS[key].label || key, group: 'Other', values: values(key), differs: true });
  return rows;
}

export const useTabLoaded = (type: PresetType) => {
  const [ready, setReady] = useState(!!TABS[TAB[type]]);
  useEffect(() => void loadTab(TAB[type]).then(() => setReady(true), () => setReady(true)), [type]);
  return ready;
};
