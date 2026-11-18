import { useMemo, useState } from 'react';
import { GroupHead, isGroupOpen, markOf, useSettingsGroups, type Mark } from '../components/SettingsGroup';
import type { EditorView } from '@codemirror/view';
import type { Config, ConfigValue, PresetType } from '../api/types';
import { normalizeConfigValue } from '../config-values';
import { OPTIONS } from '../options';
import { resetOption, setOption } from '../state/store';
import { GcodeEditor, openSearchPanel } from './GcodeEditor';
import './gcode.css';
import { copyToClipboard } from '../clipboard';
import { sameValue } from '../state/value-equality';

type Editor = { set(key: string, value: ConfigValue): void; reset(key: string): void; sources?: Record<string, string> };
const text = (v: ConfigValue | undefined): string => Array.isArray(v) ? String(v[0] ?? '') : v == null ? '' : String(v);
const lines = (s: string) => s.trim() ? s.replace(/\n$/, '').split('\n').length : 0;

const Svg = ({ d }: { d: string }) => <svg width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" aria-hidden><path d={d} /></svg>;
const ICONS = {
  reset: 'M3 3v4h4M3.5 7A5 5 0 1 1 4 11',
  diff: 'M5 2v6M2 5h6M9 12h5M4 12h.01M11 8h.01',
  copy: 'M5 5h8v8H5zM3 11V3h8',
  find: 'M7 12a5 5 0 1 0 0-10 5 5 0 0 0 0 10zM14 14l-3.5-3.5',
};

export default function GcodeWorkbench({ type, slot = 0, keys, labels, config, system, unsaved, editor, readOnly, fill }: {
  type: PresetType; slot?: number; keys: string[]; labels: Record<string, string>; config: Config; system: Config; unsaved(key: string): boolean;
  editor?: Editor; readOnly?: boolean; fill?: boolean;
}) {
  const [selected, setSelected] = useState(keys[0]);
  const [drilled, setDrilled] = useState(false);
  const [diff, setDiff] = useState(false);
  const [view, setView] = useState<EditorView | null>(null);
  const key = keys.includes(selected) ? selected : keys[0];
  const configKeys = useMemo(() => [...new Set([...Object.keys(system), ...Object.keys(config)])], [config, system]);
  const shortLabel = (k: string) => (labels[k] || OPTIONS[k]?.label || k).replace(/\s*G-?code$/i, '') || k;
  const value = (k: string) => text(config[k] ?? system[k]);
  const template = (k: string) => text(system[k]);
  const set = editor?.set ?? ((k: string, v: ConfigValue) => setOption(type, k, v, slot));
  const reset = editor?.reset ?? ((k: string) => resetOption(type, k, slot));
  const commit = (k: string, next: string) => {
    const cur = config[k] ?? system[k];
    const v = normalizeConfigValue(k, Array.isArray(cur) ? [next, ...cur.slice(1)] as string[] : next);
    if (system[k] !== undefined && !editor && sameValue(k, v, system[k])) reset(k);
    else set(k, v);
  };
  const open = (k: string) => { setSelected(k); setDrilled(true); };
  const modified = (k: string) => markOf([k], unsaved);

  useSettingsGroups();
  if (!fill && keys.length < 3) {
    const id = `${type}/Custom G-code`, open = isGroupOpen(id, !!editor && typeof window !== 'undefined' && window.innerWidth >= 1100);
    return <section className="optgroup gcode-group" data-collapsed={open ? undefined : ''}>
      <GroupHead id={id} title="Custom G-code" modified={markOf(keys, unsaved)} open={open} />
      {open && keys.map((k) => <CompactHook key={k} k={k} label={shortLabel(k)} fullLabel={labels[k] || OPTIONS[k]?.label || k} value={value(k)} template={template(k)} modified={modified(k)} readOnly={readOnly} configKeys={configKeys}
        onCommit={(t) => commit(k, t)} onReset={() => reset(k)} />)}
    </section>;
  }
  return <div className={`gcode-bench ${fill ? 'fill' : ''}`}><div className="gcode-grid" data-drilled={drilled || undefined}>
    <div className="gcode-hooks" role="listbox" aria-label="Custom G-code">
      {keys.map((k) => { const n = lines(value(k)); return <button key={k} type="button" role="option" aria-selected={k === key} className="gcode-hook" data-modified={modified(k)} aria-description={modified(k) === 'unsaved' ? 'Edited' : modified(k) ? 'Modified' : undefined} onClick={() => open(k)}>
        <span className="gcode-hook-name">{shortLabel(k)}</span>
        <span className="gcode-hook-meta">{n ? `${n} line${n > 1 ? 's' : ''}` : 'Empty'}</span>
      </button>; })}
    </div>
    <section className="gcode-pane" aria-label={shortLabel(key)}>
      <div className="gcode-pane-head">
        <button type="button" className="btn-ghost gcode-back" onClick={() => setDrilled(false)}>Back</button>
        <h3 className="gcode-title">{shortLabel(key)}</h3>
        <div className="gcode-tools" role="toolbar" aria-label="G-code tools">
          {modified(key) === 'unsaved' && !readOnly && <button type="button" className="icon-btn" aria-label="Reset to template" onClick={() => { reset(key); setDiff(false); }}><Svg d={ICONS.reset} /></button>}
          <button type="button" className="icon-btn" aria-label="Compare with template" aria-pressed={diff} onClick={() => setDiff(!diff)}><Svg d={ICONS.diff} /></button>
          <button type="button" className="icon-btn" aria-label="Copy" onClick={() => void copyToClipboard(view?.state.doc.toString() ?? value(key))}><Svg d={ICONS.copy} /></button>
          <button type="button" className="icon-btn" aria-label="Find" onClick={() => { if (view) { openSearchPanel(view); } }}><Svg d={ICONS.find} /></button>
        </div>
      </div>
      <GcodeEditor key={key} optionKey={key} label={labels[key] || OPTIONS[key]?.label || key} value={value(key)} template={template(key)} diff={diff} readOnly={readOnly} configKeys={configKeys}
        onCommit={(t) => commit(key, t)} onView={setView} />
    </section>
  </div></div>;
}

function CompactHook({ k, label, fullLabel, value, template, modified, readOnly, configKeys, onCommit, onReset }: {
  k: string; label: string; fullLabel: string; value: string; template: string; modified: Mark; readOnly?: boolean; configKeys: readonly string[]; onCommit(t: string): void; onReset(): void;
}) {
  const [view, setView] = useState<EditorView | null>(null), [diff, setDiff] = useState(false);
  return <div className="gcode-hook-row" data-modified={modified}>
    <span className="opt-label"><span className="opt-label-text">{label}</span></span>
    <div className="gcode-field">
      <div className="gcode-tools is-small" role="toolbar" aria-label="G-code tools">
        {modified === 'unsaved' && !readOnly && <button type="button" className="reset-icon" aria-label="Reset to template" onClick={() => { onReset(); setDiff(false); }}><Svg d={ICONS.reset} /></button>}
        <button type="button" className="icon-btn" aria-label="Compare with template" aria-pressed={diff} onClick={() => setDiff(!diff)}><Svg d={ICONS.diff} /></button>
        <button type="button" className="icon-btn" aria-label="Copy" onClick={() => void copyToClipboard(view?.state.doc.toString() ?? value)}><Svg d={ICONS.copy} /></button>
        <button type="button" className="icon-btn" aria-label="Find" onClick={() => { if (view) openSearchPanel(view); }}><Svg d={ICONS.find} /></button>
      </div>
      <GcodeEditor optionKey={k} label={fullLabel} value={value} template={template} diff={diff} readOnly={readOnly} configKeys={configKeys} empty="No G-code yet" compact
        onCommit={onCommit} onView={setView} />
    </div>
  </div>;
}
