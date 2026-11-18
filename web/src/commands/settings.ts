import { selectionValues } from '../profiles/view';
import type { PresetType } from '../api/types';
import { loadTab, OPTIONS, splitKey, TABS, isHidden, tooltipsReady, type OptionMode, type TabDesc } from '../options';
import { lineMode, tabEnvironment } from '../settings-env';
import { effectiveConfig, getState, setState } from '../state/store';
import { condition, optionState, settingVisible } from '../visibility';
import { commonMode, hiddenReason } from '../settings/common';
import { isRetiredSetting } from '../profiles/capabilities';
import { display } from '../components/PresetDialogs';
import type { Command } from './registry';
import { SEP, joinParts } from '../separator';

export const TYPES: [PresetType, TabDesc['tab'], string, string][] = [
  ['machine', 'printer', 'Printer', 'printer'], ['filament', 'filament', 'Filament', 'filament'], ['process', 'process', 'Process', 'process'],
];
const MODE_NAME: Record<OptionMode, string> = { simple: 'Simple', advanced: 'Advanced', develop: 'Expert', developer: 'Develop' };

interface Entry {
  type: PresetType;
  key: string;
  page: string;
  group: string;
  label: string;
  tooltip: string;
  mode: OptionMode;
  path: string;
}

function buildIndex(): Entry[] {
  const out: Entry[] = [];
  for (const [type, tab] of TYPES) {
    const seen = new Set<string>();
    for (const page of TABS[tab]?.pages ?? []) for (const group of page.groups) for (const line of group.lines) for (const k of line.options) {
      const key = splitKey(k)[0], def = OPTIONS[key];
      if (!def || isHidden(key) || isRetiredSetting(key) || seen.has(key)) continue;
      seen.add(key);
      const label = (line.options.length > 1 || line.index_labels || line.indexed_columns ? def.label || line.label : line.label ?? def.label) || def.full_label || key;
      const tooltip = (line.tooltip && OPTIONS[line.tooltip]?.tooltip) || def.tooltip || '';
      const parts = [page.title, group.title, label].filter((p, i, a) => p && p !== a[i - 1]);
      out.push({ type, key, page: page.title, group: group.title, label, tooltip, mode: lineMode(line, k) ?? def.mode, path: parts.join(' : ') });
    }
  }
  return out;
}

export function fuzzy(pattern: string, text: string): { score: number; hits: number[] } | null {
  const p = pattern.toLowerCase(), t = text.toLowerCase();
  const hits: number[] = [];
  let score = 0, j = 0, run = 0;
  for (let i = 0; i < t.length && j < p.length; i++) {
    if (t[i] !== p[j]) { run = 0; continue; }
    const wordStart = i === 0 || /[\s:_\-(/]/.test(t[i - 1]);
    score += 10 + (run ? 15 * run : 0) + (wordStart ? 20 : 0);
    run++;
    hits.push(i);
    j++;
  }
  if (j < p.length) return null;
  score -= Math.min(30, hits[0]) + 3 * (hits[hits.length - 1] - hits[0] + 1 - hits.length);
  return { score, hits };
}

interface Found { entry: Entry; score: number; via?: 'key' | 'tooltip' }

const MODE_BOOST: Partial<Record<OptionMode, number>> = { simple: 80, advanced: 20 };
const PRIMARY: Record<string, string> = {
  layer: 'layer_height', wall: 'wall_loops', perimeter: 'wall_loops', infill: 'sparse_infill_density', density: 'sparse_infill_density',
  support: 'enable_support', supports: 'enable_support', brim: 'brim_type', raft: 'raft_layers', skirt: 'skirt_loops', seam: 'seam_position',
  ironing: 'ironing_type', top: 'top_shell_layers', bottom: 'bottom_shell_layers', temperature: 'nozzle_temperature', temp: 'nozzle_temperature',
  bed: 'curr_bed_type', retraction: 'retraction_length', fan: 'fan_max_speed', speed: 'outer_wall_speed', vase: 'spiral_mode', spiral: 'spiral_mode',
};

function search(index: Entry[], query: string): Found[] {
  const q = query.trim();
  if (!q) return [];
  const words = q.toLowerCase().split(/\s+/);
  const stem = q.length > 3 && /[^s]s$/i.test(q) ? q.slice(0, -1).toLowerCase() : '';
  const found: Found[] = [];
  for (const entry of index) {
    const own = fuzzy(q, entry.label), path = fuzzy(q, entry.path);
    const label = own && (!path || own.score >= path.score) ? own : path;
    const lower = entry.label.toLowerCase();
    let at = lower.indexOf(q.toLowerCase());
    if (at < 0 && stem) at = lower.indexOf(stem);
    const bonus = at < 0 ? 0 : 120 + (at === 0 ? 60 : 0) - entry.label.length + (MODE_BOOST[entry.mode] ?? 0);
    let best: Found | null = label && (label.score > 90 || at >= 0 || q.length < 4 && label.score > 40) ? { entry, score: label.score + bonus } : null;
    const key = entry.key.toLowerCase();
    if (words.every((w) => key.includes(w)) && (!best || best.score < 150)) best = { entry, score: 150 + (key === q.toLowerCase() ? 100 : 0), via: 'key' };
    if (!best && q.length >= 3 && words.every((w) => entry.tooltip.toLowerCase().includes(w))) best = { entry, score: 40, via: 'tooltip' };
    if (best && (PRIMARY[q.toLowerCase()] ?? PRIMARY[stem]) === entry.key) best.score += 400;
    if (best) found.push(best);
  }
  return found.sort((a, b) => b.score - a.score || a.entry.path.localeCompare(b.entry.path)).slice(0, 40);
}

function hiddenBySettings(entry: Entry, tab: TabDesc['tab']): string | undefined {
  const s = getState();
  const slot = s.dialog?.type === 'filament' ? s.dialog.slot : 0;
  const env = tabEnvironment(s, entry.type, slot, 'developer');
  const desc = TABS[tab]!;
  const page = desc.pages.find((p) => p.title === entry.page);
  const group = page?.groups.find((g) => g.title === entry.group);
  if ((page?.condition && condition(page.condition, env) === false) || (group?.condition && condition(group.condition, env) === false)
    || optionState({ ...desc, visibility: desc.visibility.filter((r) => r.options.some((o) => splitKey(o)[0] === entry.key)) }, env)[entry.key]?.visible === false)
    return 'hidden by settings';
  return undefined;
}

function reveal(entry: Entry) {
  const s = getState();
  const patch: Parameters<typeof setState>[0] = { presetUx: null };
  if (entry.type === 'process') Object.assign(patch, { dialog: null, tab: 'prepare' });
  else if (s.dialog?.type !== entry.type) patch.dialog = { type: entry.type, slot: 0 };
  setState({ ...patch, settingsFocus: { type: entry.type, page: entry.page, key: entry.key, nonce: Date.now() } });
}

function valueOf(e: Entry): string {
  const s = getState(), slot = e.type === 'filament' && s.dialog?.type === 'filament' ? s.dialog.slot : 0;
  if (selectionValues(s, e.type, slot).state !== 'ready') return '';
  const v = effectiveConfig(s, e.type, slot)[e.key];
  if (v === undefined || OPTIONS[e.key]?.multiline || /gcode|filament(_id)?$/.test(e.key)) return '';
  const text = OPTIONS[e.key]?.type.startsWith('bool') ? (display(e.key, v as never) === 'true' ? 'On' : 'Off') : display(e.key, v as never);
  const unit = OPTIONS[e.key]?.sidetext;
  return text.length > 24 ? '' : unit && !/^[a-z]/i.test(text) && !/%$/.test(text) && !text.endsWith(unit) && !/ or /.test(unit) ? `${text} ${unit}` : text;
}

let index: Entry[] | null = null;
let loading: Promise<Entry[]> | null = null;
export const settingsLoaded = () => !!index;
export function loadSettings(): Promise<Entry[]> {
  return loading ??= Promise.all(TYPES.map(([, tab]) => loadTab(tab))).then(tooltipsReady).then(() => (index = buildIndex()));
}

export async function settingCommands(query: string): Promise<Command[]> {
  const all = await loadSettings();
  const s = getState();
  return search(all, query).map((f, i) => {
    const { entry } = f;
    const shown = settingVisible(entry.type, entry.key, s, entry.mode);
    const tab = TYPES.find(([t]) => t === entry.type)!;
    const reason = shown ? hiddenBySettings(entry, tab[1])
      : hiddenReason(entry.key, s, entry.type) ?? MODE_NAME[commonMode(entry.type, entry.key, s, entry.mode)];
    return {
      id: `setting.${entry.type}.${entry.key}`,
      title: entry.label,
      group: 'Setting',
      detail: joinParts([valueOf(entry), `${tab[2]}${SEP}${entry.page}`, reason]),
      keywords: `${entry.path} ${entry.key}`,
      boost: (shown ? 200 : 0) + (f.via === 'tooltip' ? -150 : 0) - i,
      run: () => reveal(entry),
    };
  });
}
