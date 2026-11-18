import { useMemo, useState } from 'react';
import { Dropdown } from '../components/Dropdown';
import { OPTIONS, splitKey, TABS } from '../options';
import { useTabLoaded } from '../components/PresetDialogs';
import { Glyph } from '../components/Glyph';
import { effectiveShells, heightsFor, scaledValue, type ScaleBasis, type ScaleRow } from './auto-rules';
import type { Config } from '../api/types';
import { SPEED_FEATURES, SPEED_KEYS, type SpeedModel, type SpeedDetail } from './speed-rules';
import { labelOf, overrideText } from './PrintOverrides';
import type { PrinterRecord } from './records';
import { SEP } from '../separator';

const GROUPS: { name: string; basis: ScaleBasis; keys: string[] }[] = [
  { name: 'Line widths', basis: 'nozzle', keys: ['line_width', 'inner_wall_line_width', 'sparse_infill_line_width', 'internal_solid_infill_line_width', 'top_surface_line_width', 'support_line_width'] },
  { name: 'Support Z gap', basis: 'layer', keys: ['support_top_z_distance', 'support_bottom_z_distance'] },
];
const NAMES: Record<string, string> = {
  outer_wall_line_width: 'Outer wall', initial_layer_line_width: 'First layer width', initial_layer_print_height: 'First layer height',
  top_shell_layers: 'Top shell layers', bottom_shell_layers: 'Bottom shell layers', inner_wall_line_width: 'Inner wall width',
  sparse_infill_line_width: 'Sparse infill width', internal_solid_infill_line_width: 'Solid infill width', top_surface_line_width: 'Top surface width',
  support_line_width: 'Support width', support_top_z_distance: 'Support top Z gap', support_bottom_z_distance: 'Support bottom Z gap', line_width: 'Default line width',
};
const nameOf = (key: string) => NAMES[key] ?? OPTIONS[key]?.label ?? key;
const NOZZLES = [0.2, 0.4, 0.6, 0.8];
const BASES: { id: ScaleBasis; label: string }[] = [{ id: 'nozzle', label: '× nozzle' }, { id: 'layer', label: '× layer height' }, { id: 'perLayer', label: '÷ layer height' }];
const mm = (v: number) => String(Math.round(v * 1000) / 1000);
const SPEED_HEADS: [string, string, boolean][] = [['Base mm/s', 'From the profile', true], ['Multiplier', 'Your adjustment', true], ['Speed mm/s', 'Actual print speed', true], ['Limit', 'Speed limit applied', false], ['Accel. mm/s²', 'Acceleration', true]];
const isWidth = (key: string) => key === 'line_width' || key.endsWith('_line_width');

interface Formula { name: string; keys: string[]; basis: ScaleBasis; factor: number }
const SCALAR = new Set(['float', 'int', 'percent', 'float_or_percent']);
interface Section { title: string; keys: string[] }
function numericSections(): Section[] {
  return (TABS.process?.pages ?? []).flatMap((page) => page.groups.map((g) => ({ title: `${page.title}${SEP}${g.title}`,
    keys: [...new Set(g.lines.flatMap((l) => l.options.map((k) => splitKey(k)[0])))].filter((k) => k !== 'layer_height' && !k.endsWith('_speed') && !k.endsWith('_acceleration') && OPTIONS[k]?.label && SCALAR.has(OPTIONS[k].type)) })))
    .filter((sec) => sec.keys.length);
}

function SettingFinder({ exclude, onPick, onCancel }: { exclude: ReadonlySet<string>; onPick(key: string): void; onCancel(): void }) {
  const loaded = useTabLoaded('process'), [query, setQuery] = useState('');
  const sections = useMemo(() => (loaded ? numericSections() : []), [loaded]);
  const q = query.trim().toLowerCase();
  const shown = sections.map((sec) => ({ ...sec, keys: sec.keys.filter((k) => !exclude.has(k) && (!q || `${nameOf(k)} ${sec.title} ${k}`.toLowerCase().includes(q))) })).filter((sec) => sec.keys.length);
  return <div className="dyn-finder">
    <input className="text" type="search" autoFocus aria-label="Find a setting" placeholder="Find a setting" value={query} onChange={(e) => setQuery(e.target.value)}
      onKeyDown={(e) => { if (e.key === 'Escape') { e.stopPropagation(); onCancel(); } else if (e.key === 'Enter' && shown[0]) onPick(shown[0].keys[0]); }}/>
    <div className="dyn-finder-list" role="listbox" aria-label="Settings">
      {!loaded ? <span className="dyn-finder-empty">Loading…</span> : !shown.length ? <span className="dyn-finder-empty">No setting matches</span>
        : shown.map((sec) => <div key={sec.title} role="group" aria-label={sec.title}><div className="dyn-finder-group">{sec.title}</div>
          {sec.keys.map((k) => <button key={k} type="button" role="option" aria-selected={false} className="dyn-finder-item" onClick={() => onPick(k)}>{nameOf(k)}</button>)}</div>)}
    </div>
    <button type="button" className="btn-ghost" onClick={onCancel}>Cancel</button>
  </div>;
}
function formulasOf(rows: ScaleRow[], split: ReadonlySet<string>): Formula[] {
  const out: Formula[] = [], used = new Set<string>(), named = new Set<string>();
  for (const row of rows) {
    if (used.has(row.key)) continue;
    const group = GROUPS.find((g) => g.basis === row.basis && g.keys.includes(row.key) && !split.has(row.key));
    const members = group ? rows.filter((r) => r.basis === group.basis && group.keys.includes(r.key) && !split.has(r.key) && r.factor === row.factor) : [row];
    members.forEach((m) => used.add(m.key));
    const name = !group || members.length < 2 ? nameOf(row.key) : named.has(group.name) ? members.map((m) => nameOf(m.key)).join(', ') : group.name;
    if (group) named.add(group.name);
    out.push({ name, keys: members.map((m) => m.key), basis: row.basis, factor: row.factor });
  }
  return out;
}

function FormulaLine({ f, nozzle, height, readOnly, onFactor, onRemove }: { f: Formula; nozzle: number; height: number; readOnly?: boolean; onFactor(v: number): void; onRemove(): void }) {
  const v = scaledValue({ key: f.keys[0], basis: f.basis, factor: f.factor }, { nozzle, layerHeight: height });
  const hint = f.basis === 'nozzle' ? `${mm(v)} mm on the ${nozzle} mm nozzle` : f.basis === 'layer' ? `${mm(v)} mm at ${height} mm layers` : `${v} at ${height} mm layers`;
  const input = <input className="text num dyn-factor" type="number" step="0.01" min="0.01" aria-label={`${f.name} factor`} disabled={readOnly} value={f.factor}
    onChange={(e) => { const v = Number(e.target.value); if (v > 0) onFactor(v); }}/>;
  return <li className="dyn-formula">
    <span className="dyn-name">{f.name}</span><span className="dyn-eq">=</span>
    <span className="dyn-expr"><span className="dyn-ceil" data-hidden={f.basis !== 'perLayer' || undefined}>⌈</span>{input}
      {f.basis === 'perLayer' ? 'mm ÷ layer height ⌉' : `× ${f.basis === 'nozzle' ? 'nozzle' : 'layer height'}`}</span>
    <span className="hint-text">{hint}</span>{!readOnly && <button type="button" className="reset-icon row-remove" aria-label="Remove formula" onClick={onRemove}><Glyph name="trash" size={14}/></button>}
  </li>;
}

export function DynamicTab({ speedModel, speeds: firstSpeeds = {}, nozzleSpeeds, missingReference, slowerNote, onSpeedModel, onUseMultiplier, rows, nozzle, height, minHeight, maxHeight, readOnly, onChange, fromPrinter, config = {} }: {
  speedModel?: SpeedModel; speeds?: Record<string, SpeedDetail>;  nozzleSpeeds?: { label: string; speeds: Record<string, SpeedDetail> }[]; missingReference?: boolean;  slowerNote?: boolean; onSpeedModel?(model: SpeedModel): void; onUseMultiplier?(key: string): void;
  rows: ScaleRow[]; nozzle: number; height: number; minHeight?: number; maxHeight?: number; readOnly?: boolean; onChange(rows: ScaleRow[]): void;
   config?: Config;
   fromPrinter?: { name: string; values: Config; printer?: PrinterRecord };
}) {
  const [split, setSplit] = useState<ReadonlySet<string>>(() => new Set());
  const [side, setSide] = useState(0), speeds = nozzleSpeeds?.[side]?.speeds ?? firstSpeeds;
  const [adding, setAdding] = useState<{ key: string; basis: ScaleBasis } | null>(null);
  const [folded, setFolded] = useState<ReadonlySet<number>>(() => new Set());
  const formulas = formulasOf(rows.filter(r => !SPEED_KEYS.has(r.key)), split);
  const setFactor = (keys: string[], factor: number) => onChange(rows.map((r) => (keys.includes(r.key) ? { ...r, factor } : r)));
  const remove = (keys: string[]) => onChange(rows.filter((r) => !keys.includes(r.key)));
  const add = () => {
    if (!adding) return;
    const existing = rows.find((r) => r.key === adding.key);
    setSplit(new Set([...split, adding.key]));
    if (!existing) onChange([...rows, { key: adding.key, basis: adding.basis, factor: 1 }]);
    else if (existing.basis !== adding.basis) onChange(rows.map((r) => (r.key === adding.key ? { ...r, basis: adding.basis, factor: 1 } : r)));
    setAdding(null);
  };
  const own = new Set(formulas.filter((f) => f.keys.length === 1).map((f) => f.keys[0]));
  const cell = (f: Formula, n: number, h: number) => {
    const row: ScaleRow = { key: f.keys[0], basis: f.basis, factor: f.factor }, v = scaledValue(row, { nozzle: n, layerHeight: h });
    const raw = f.basis === 'nozzle' ? f.factor * n : f.basis === 'layer' ? f.factor * h : v, clamped = f.basis !== 'perLayer' && Math.abs(raw - v) > 1e-6;
    const why = isWidth(row.key) ? 'Automatically kept within 90–150% of your nozzle size' : 'Automatically kept at or below 75% of your nozzle size';
    return <td key={f.keys.join()} className="n" {...(clamped ? { 'data-clamped': true, 'data-truncated': 'true', 'data-full-text': why } : {})}>{f.basis === 'perLayer' ? v : mm(v)}</td>;
  };
  const shells = (n: number, h: number) => { const row = rows.find((r) => r.key === 'top_shell_layers');
    const e = effectiveShells(row ? { ...config, top_shell_layers: String(scaledValue(row, { nozzle: n, layerHeight: h })) } : config, 'top', h);
    return <td className="n" {...(e.lifted ? { 'data-clamped': true, 'data-truncated': 'true', 'data-full-text': `${e.layers} raised to keep ≥ ${e.thickness} mm` } : {})}>{e.effective}</td>; };
  return <section className="dyn-tab" aria-label="Dynamic">
    {speedModel && <section className="dyn-speeds" aria-label="Dynamic speeds">
      <div className="dyn-speed-controls">
        <label>Outer-wall ceiling <span><input className="text num" type="number" min="1" max="2000" step="5" aria-label="Outer-wall ceiling" value={speedModel.outerWallCeiling} disabled={readOnly}
          onChange={e => {const n = Number(e.target.value); if (n > 0 && n <= 2000) onSpeedModel?.({...speedModel, outerWallCeiling: n});}}/> mm/s</span></label>
        {nozzleSpeeds && nozzleSpeeds.length > 1 && <label>Values for <Dropdown className="combo" aria-label="Speeds for nozzle" value={String(Math.min(side, nozzleSpeeds.length - 1))}
          options={nozzleSpeeds.map((n, i) => ({ value: String(i), label: n.label }))} onValueChange={(v) => setSide(Number(v))}/></label>}
      </div>
      {!missingReference && slowerNote && <p className="hint-text">Slower filaments will cruise at their own flow limit.</p>}
      {!missingReference && <p className="hint-text">Speed = printer base speed × multiplier (automatically capped by your hotend's max flow rate and printer speed limits). Adjust any multiplier to tune print speed.</p>}
      {missingReference ? <p role="status">This printer doesn't have base speed values for dynamic profiles yet—pick a regular print profile instead.</p> : <div className="dyn-speed-scroll"><table className="dyn-table dyn-speed-table">
        <thead><tr><th>Feature</th>{SPEED_HEADS.map(([label, tip, n]) => <th key={label} className={n ? 'n' : undefined} data-truncated="true" data-full-text={tip}>{label}</th>)}</tr></thead>
        <tbody>{SPEED_FEATURES.map(f => {const d = speeds[f.key]; if (!d) return null; return <tr key={f.key}>
          <th scope="row">{f.label}</th><td className="n">{Math.round(d.base)}</td>
          <td className="n"><input className="text num dyn-factor" type="number" step="0.05" min="0.01" max="10" aria-label={`${f.label} multiplier`} disabled={readOnly || !!d.override} value={d.multiplier.toFixed(2)}
            onChange={e => {const n = Number(e.target.value); if (n > 0 && n <= 10) onSpeedModel?.({...speedModel, multipliers: {...speedModel.multipliers, [f.key]: n}});}}/></td>
          {d.limit ? <td className="n" data-capped data-truncated="true" data-full-text={`Capped from ${Math.round(d.requested)} by ${d.limit.toLowerCase()}`}>↓ {Math.round(d.speed)}</td> : <td className="n">{Math.round(d.speed)}</td>}<td><span>{d.limit || 'None'}</span>{d.override && <span className="dyn-speed-override">Absolute override ({Math.round(d.requested)} mm/s){!readOnly && onUseMultiplier && <button type="button" className="btn-ghost" onClick={() => onUseMultiplier(f.key)}>Use multiplier</button>}</span>}</td>
          <td className="n">{mm(d.acceleration)}</td>
        </tr>;})}</tbody>
      </table></div>}
    </section>}

    <div className="dyn-rows"><ul className="dyn-formulas">
      {formulas.map((f) => <FormulaLine key={f.keys.join(',')} f={f} nozzle={nozzle} height={height} readOnly={readOnly} onFactor={(v) => setFactor(f.keys, v)} onRemove={() => remove(f.keys)}/>)}
      {!readOnly && <li className="dyn-formula dyn-add">{adding
        ? !adding.key ? <SettingFinder exclude={own} onPick={(key) => setAdding({ key, basis: rows.find((r) => r.key === key)?.basis ?? (isWidth(key) ? 'nozzle' : 'layer') })} onCancel={() => setAdding(null)}/>
          : <><span className="dyn-name">{nameOf(adding.key)}</span>
          <Dropdown className="combo" aria-label="Basis" value={adding.basis} options={BASES.map((b) => ({ value: b.id, label: b.label }))} onValueChange={(basis) => setAdding({ ...adding, basis: basis as ScaleBasis })}/>
          <button type="button" className="btn" onClick={add}>Add</button><button type="button" className="btn-ghost" onClick={() => setAdding(null)}>Cancel</button></>
        : <button type="button" className="link" onClick={() => setAdding({ key: '', basis: 'nozzle' })}>+ Add formula</button>}</li>}
    </ul>
    {fromPrinter && Object.keys(fromPrinter.values).length > 0 && <ul className="dyn-formulas dyn-printer" aria-label={`From ${fromPrinter.name}`}>
      <li className="dyn-group">From {fromPrinter.name}</li>
      {Object.entries(fromPrinter.values).filter(([k]) => !speedModel || !SPEED_KEYS.has(k) && !SPEED_FEATURES.some(f => f.acceleration === k)).map(([k, v]) => <li key={k} className="dyn-formula"><span className="dyn-name">{labelOf(k)}</span><span className="dyn-eq">=</span><span className="dyn-expr"><span className="dyn-ceil" data-hidden>⌈</span>{overrideText(k, v, config, fromPrinter.printer)}{OPTIONS[k]?.sidetext ? ` ${OPTIONS[k]?.sidetext}` : ''}</span></li>)}
    </ul>}</div>
    <div className="dyn-table-scroll"><table className="dyn-table">
      <thead><tr><th>Layer</th>{formulas.map((f) => <th key={f.keys.join()} className="n">{f.name}</th>)}<th className="n">Top shells</th></tr></thead>
      {NOZZLES.map((n) => {
        const own = Math.abs(n - nozzle) < 1e-6, heights = heightsFor(n, own ? minHeight : Math.ceil(n * 0.25 / 0.04 - 1e-6) * 0.04, own ? maxHeight : undefined).filter((h) => h <= n * 0.75 + 1e-6), open = !folded.has(n);
        return <tbody key={n}>
          <tr className="dyn-nozzle"><th colSpan={formulas.length + 2}><button type="button" aria-expanded={open} onClick={() => setFolded(open ? new Set([...folded, n]) : new Set([...folded].filter((x) => x !== n)))}>{n} mm nozzle</button></th></tr>
          {open && heights.map((h) => <tr key={h} className={Math.abs(n - nozzle) < 1e-6 && Math.abs(h - height) < 1e-6 ? 'is-current' : undefined}>
            <td className="n">{h.toFixed(2)}</td>{formulas.map((f) => cell(f, n, h))}{shells(n, h)}
          </tr>)}
        </tbody>;
      })}
    </table></div>
  </section>;
}
