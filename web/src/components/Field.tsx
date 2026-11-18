import { Check } from './Check';
import { ColorPicker } from './ColorPicker';
import { Dropdown, EditableDropdown } from './Dropdown';
import { createContext, memo, useContext, useEffect, useId, useRef, useState } from 'react';
import type { ConfigValue, PresetType } from '../api/types';
import { normalizeConfigValue } from '../config-values';
import { sameValue } from '../state/value-equality';
import { DESKTOP_ONLY, OPTIONS, splitKey, useTooltips, type OptionDef } from '../options';
import { effectiveConfig, getState, resetOption, setOption, toast, useStore } from '../state/store';
import { fieldIssue, fieldNumber, fieldRange } from './field-validation';
import { Glyph } from './Glyph';
import { MODE_HELP, MODE_LABELS } from '../nonplanar-mode';
import { LabelTip, SourceGlyph } from './InfoTip';
import { displayLabel } from './fields/labels';
import { display } from '../profiles/format';
import { ScopeMark } from '../profiles/Scope';
import type { ChangeTarget } from '../state/changes';
import { slotFilaments } from '../profiles/slots';

const LabelId = createContext<string | undefined>(undefined);
const useLabel = () => ({ 'aria-labelledby': useContext(LabelId) });

const NUMERIC = new Set(['float', 'floats', 'int', 'ints', 'percent', 'percents', 'float_or_percent', 'floats_or_percents']);

function bounded(def: OptionDef): OptionDef {
  if (def.sidetext?.trim() !== 'mm/s' || !def.key.endsWith('_speed')) return def;
  const machine = effectiveConfig(getState(), 'machine');
  const top = Math.max(...['machine_max_speed_x', 'machine_max_speed_y'].map((k) => Number([machine[k]].flat()[0]) || 0));
  return top > 0 && (def.max === undefined || top < def.max) ? { ...def, max: top } : def;
}
function rejectField(text: string, previous: string, def: OptionDef, label: string): boolean {
  const own = def; def = bounded(def);
  const issue = fieldIssue(text, def);
  if (!issue) return false;
  if (def.max !== own.max && Number(text) > def.max!) {
    toast('printer_tops_out_mm', { max: def.max, previous });
    return true;
  }
  toast('kept_instead', { label, value: fieldNumber(text, def), detail: issue.detail, value2: fieldNumber(previous, def) });
  return true;
}

function elementOf(v: ConfigValue | undefined, idx: number, vector: boolean): ConfigValue | undefined {
  if (!vector) return v;
  if (Array.isArray(v)) return v[idx] ?? v[0];
  return v;
}

function withElement(v: ConfigValue | undefined, idx: number, el: ConfigValue): ConfigValue {
  const arr = Array.isArray(v) ? [...v] : [];
  while (arr.length <= idx) arr.push(arr[0] ?? '');
  arr[idx] = el as string;
  return arr as string[];
}

const asText = (v: unknown): string => v == null ? '' : Array.isArray(v) ? v.map(asText).join(', ') : typeof v === 'object' && 'value' in v ? `${v.value}${'percent' in v && v.percent ? '%' : ''}` : String(v);

const CHOICE_HINTS: Record<string, Record<string, string>> = {
  nonplanar_mode: {
    ...MODE_HELP,
    multiplanar: 'Tilted chunks: tilts the print bed for each section so overhangs print cleanly upright.',
    field: 'Rotary curved layers: curves every layer while the rotary table tilts to keep the nozzle level.',
  },
};

function HelpText({text, current}: {text: string; current?: string}) {
  const lines = text.split('\n').filter(Boolean), items = lines.map((l) => /^([^:]{1,40}): (.+)$/.exec(l));
  if (lines.length < 2 || items.some((m) => !m)) return <p>{text}</p>;
  return <ul className="info-choices">{items.map((m, i) => <li key={i} aria-current={lines[i] === current || undefined}><b>{m![1]}</b> {m![2]}</li>)}</ul>;
}
function defaultText(def: OptionDef): string | undefined {
  const raw = def.default === undefined ? '' : asText(def.default);
  if (raw === '') return undefined;
  const at = def.enum_values?.indexOf(raw) ?? -1;
  if (at >= 0) return (def.key === 'nonplanar_mode' ? MODE_LABELS[raw] : undefined) ?? def.enum_labels?.[at] ?? raw;
  return fieldNumber(raw, def);
}

const PROMPTED = new Set(['spiral_mode', 'wall_loops', 'top_shell_layers', 'sparse_infill_density', 'enable_support', 'enforce_support_layers', 'detect_thin_wall',
  'overhang_reverse', 'timelapse_type', 'enable_wrapping_detection', 'layer_height', 'nozzle_temperature', 'nozzle_temperature_initial_layer',
  'nozzle_temperature_range_low', 'nozzle_temperature_range_high', 'alternate_extra_wall', 'ensure_vertical_shell_thickness', 'seam_slope_type',
  'seam_slope_start_height', 'filament_max_volumetric_speed']);

const sameProp = (a: unknown, b: unknown) => a === b || (Array.isArray(a) && Array.isArray(b) && a.length === b.length && a.every((x, i) => x === b[i]));
const sameFieldProps = (a: Record<string, unknown>, b: Record<string, unknown>) => {
  const keys = Object.keys(a);
  return keys.length === Object.keys(b).length && keys.every((k) => sameProp(a[k], b[k]));
};
export const Field = memo(function Field(props: {
  optKey: string;
  type: PresetType;
  slot?: number;
  value: ConfigValue | undefined;
  systemValue: ConfigValue | undefined;
  dirty: boolean;
  disabled?: boolean;
  label?: string;
  hideLabel?: boolean;
  tooltip?: string;
  definition?: Partial<OptionDef>;
  inheritedValue?: ConfigValue;
  overrideDisabled?: boolean;
  overrideChecked?: boolean;
  onSet?: (key: string, value: ConfigValue) => void;
  onReset?: (key: string) => void;
  resetTitle?: string;
  extra?: React.ReactNode;
  scope?: { target?: ChangeTarget };
  widget?: React.ReactNode;
  source?: 'auto' | 'set' | 'printer';
  sourceText?: string;
  mixed?: string;
  overriddenBy?: string;
  mode?: string;
}) {
  useTooltips();
  const [unmixed, setUnmixed] = useState(false);
  const fieldRef = useRef<HTMLFieldSetElement>(null);
  useEffect(() => setUnmixed(false), [props.mixed]);
  const [key, idx] = splitKey(props.optKey);
  const labelId = useId();
  const baseDef = OPTIONS[key];
  if (!baseDef) return null;
  const def: OptionDef = { ...baseDef, ...props.definition, min: baseDef.min, max: (props.mode ? baseDef.max_by_mode?.[props.mode] : undefined) ?? baseDef.max };
  const vector = def.type.endsWith('s') && (props.optKey.includes('#') || !['points', 'points_groups', 'ints_groups'].includes(def.type));
  const current = props.value === undefined ? undefined : normalizeConfigValue(key, props.value);
  const system = props.systemValue === undefined ? undefined : normalizeConfigValue(key, props.systemValue);
  const value = elementOf(current, idx, vector);
  const prompt = (k: string) => { if (PROMPTED.has(k)) void import('../configPrompts').then((m) => m.checkSetting(props.type, k, props.slot)); };
  const set = props.onSet ?? ((k: string, v: ConfigValue) => { setOption(props.type, k, v, props.slot); prompt(k); });
  const reset = props.onReset ?? ((k: string) => { resetOption(props.type, k, props.slot); prompt(k); });
  const commit = (el: ConfigValue) => {
    const next = normalizeConfigValue(key, vector ? withElement((current ?? system)!, idx, el) : el);
    if (system !== undefined && sameValue(key, next, system) && !props.onSet) reset(key);
    else set(key, next);
  };
  const fullLabel = props.label ?? (def.label || def.full_label || key);
  const label = props.label === undefined ? displayLabel(key, fullLabel) : fullLabel;
  const modified = !!props.dirty;
  const sourced = props.source === 'auto' || props.source === 'printer', glyph = <SourceGlyph text={props.sourceText}/>;
  const glyphInUnit = sourced && !props.widget && NUMERIC.has(def.type) && !def.multiline && !(props.mixed && !unmixed);
  const checked = !props.overrideDisabled && (props.overrideChecked ?? (value != null && value !== 'nil' && value !== ''));
  const inherited = props.inheritedValue;
  const real = (v: ConfigValue | undefined) => v != null && v !== 'nil' && v !== '' ? v : undefined;
  const fallback = real(value) ?? real(elementOf(system, idx, vector)) ?? real(elementOf(inherited as ConfigValue | undefined, idx, vector)) ?? String(def.min !== undefined && Math.abs(def.min) < 1e9 ? def.min : 0);
  const isSwitch = def.type.replace(/s$/, '') === 'bool' && !FILAMENT_SLOT_KEYS.has(key);
  const isOn = value === true || value === 'true' || value === '1' || value === 1;
  const locked = !!props.disabled || !!def.readonly || (def.nullable && !checked);
  const disabledReason = def.readonly ? 'This setting is locked in the current profile.' : props.disabled ? 'This setting isn\'t available with your current printer and print profile combination.' : def.nullable && !checked ? 'Turn on the override toggle first to customize this value.' : undefined;
  const hint = CHOICE_HINTS[key]?.[asText(value)];
  const help = props.tooltip || def.tooltip;

  const was = system !== undefined ? `Was ${display(key, elementOf(system, idx, vector) ?? system)}` : (props.resetTitle ?? 'Reset');
  return (
    <div className={`opt-line ${def.full_width || def.multiline ? 'full' : ''} ${isSwitch ? 'is-switch' : ''}`} data-opt={key} data-source={props.source} data-modified={modified ? 'unsaved' : undefined} onClick={isSwitch && !locked ? (e) => {
      if (!(e.target as HTMLElement).closest('input, button')) commit(isOn ? '0' : '1');
    } : undefined}>
      <span className={`opt-label ${modified ? 'modified' : ''} ${props.hideLabel ? 'field-label-hidden' : ''}`} id={labelId}>
        {def.nullable && <Check className="nullable-toggle" aria-label={`Override ${label}`} checked={checked} disabled={props.overrideDisabled}
        onCheckedChange={(next) => { if (!next) commit('nil'); else if (!rejectField(asText(fallback), asText(value ?? ''), def, label)) commit(fallback); }} />}
        {props.hideLabel ? <span className="opt-label-text">{label}</span> : <LabelTip label={label} full={fullLabel}>
          <strong>{fullLabel}</strong>{hint && !help?.includes(hint) && <p>{hint}</p>}{help && <HelpText text={help} current={hint}/>}
          {defaultText(def) && <p className="info-meta">Default {defaultText(def)}</p>}
          {!def.enum_values?.length && fieldRange(def) && <p className="info-meta">{fieldRange(def)}</p>}
          <code>{key}</code></LabelTip>}
      {props.overriddenBy && <span className="opt-object-dot" role="img" aria-label={`Also set on ${props.overriddenBy}`} title={`Also set on ${props.overriddenBy}`}/>}</span>
      <fieldset ref={fieldRef} className="opt-field" aria-description={disabledReason} disabled={props.disabled || (def.nullable && !checked)}>
        <LabelId.Provider value={labelId}>
          {props.widget ?? (props.mixed && !unmixed
            ? <button type="button" className="field-mixed" aria-label={`${label}: mixed values ${props.mixed}; set one value`}
              onClick={() => { setUnmixed(true); requestAnimationFrame(() => fieldRef.current?.querySelector<HTMLElement>('input, button, [role=combobox]')?.focus()); }}>
              <span>Mixed</span><span className="field-mixed-values">{props.mixed}</span></button>
            : <Widget def={def} label={label} value={def.nullable && !checked ? fallback : value} onCommit={commit} glyph={glyphInUnit ? glyph : undefined} />)}
        </LabelId.Provider>
        {def.sidetext && (props.widget || !NUMERIC.has(def.type)) && <span className="opt-side">{def.sidetext}</span>}
        {modified && !props.scope && <button type="button" className="reset-icon field-reset" title={was} aria-label={`${label}: ${was}`}
          onClick={() => reset(key)}><Glyph name="undo" size={14} /></button>}
      </fieldset>
      <span className="opt-undo">
        {sourced && !glyphInUnit && glyph}
        {props.scope ? <ScopeMark type={props.type} slot={props.slot} optKey={key} target={props.scope.target} /> : props.extra}
      </span>
    </div>
  );
}, sameFieldProps);

const FILAMENT_SLOT_KEYS = new Set(['support_filament', 'support_interface_filament', 'wipe_tower_filament', 'outer_wall_filament_id',
  'inner_wall_filament_id', 'sparse_infill_filament_id', 'internal_solid_filament_id', 'top_surface_filament_id', 'bottom_surface_filament_id']);

function SlotChoice({ value, onCommit }: { value: string; onCommit: (v: ConfigValue) => void }) {
  const slots = useStore((s) => slotFilaments(s));
  useStore((s) => s.filamentEdits);
  const type = (i: number) => { const t = effectiveConfig(getState(), 'filament', i).filament_type; return String(Array.isArray(t) ? t[0] : t ?? ''); };
  const known = value === '0' || (Number(value) >= 1 && Number(value) <= slots.length);
  return (
    <Dropdown className="combo" {...useLabel()} value={value} onValueChange={onCommit} options={[{ value: "0", label: 'Default' }, ...slots.map((_, i) => ({ value: String(i + 1), label: <>{i + 1}: {type(i)}</> })), ...(!known ? [{ value, label: value }] : [])]} />
  );
}

function Widget({ def, label, value, onCommit, glyph }: { def: OptionDef; label: string; value: ConfigValue | undefined; onCommit: (v: ConfigValue) => void; glyph?: React.ReactNode }) {
  const named = useLabel();
  const base = def.type.replace(/s$/, '');
  if (FILAMENT_SLOT_KEYS.has(def.key)) return <SlotChoice value={asText(value) || '0'} onCommit={onCommit} />;
  if (base === 'bool') {
    const on = value === true || value === 'true' || value === '1' || value === 1;
    return (
      <Check {...named} checked={on} disabled={def.readonly} onCheckedChange={(v) => onCommit(v ? '1' : '0')} />
    );
  }
  if (def.enum_values?.length && /^(select_open|[if]_enum_open)$/.test(def.gui_type ?? '')) {
    if (NUMERIC.has(def.type)) return <OpenNumber def={def} label={label} value={asText(value)} onCommit={onCommit} />;
    return <EditableDropdown {...named} value={asText(value)} readOnly={def.readonly} onValueChange={onCommit} className="text" options={def.enum_values.map((v,i) => ({value: v, label: def.enum_labels?.[i] ?? v}))} />;
  }
  if ((base === 'enum' || def.enum_values) && def.enum_values?.length) {
    const v = asText(value);
    const known = def.enum_values.includes(v);
    const labels = def.enum_values.map((ev, i) => def.enum_labels?.[i] ?? ev);
    return (
      <Dropdown className="combo" {...named} value={v} disabled={def.readonly} onValueChange={onCommit} options={[...(!known ? [{ value: v, label: v }] : []), ...def.enum_values.map((ev, i) => (
          ({ value: ev, label: def.enum_notes?.[i] ? <>{labels[i]}<span className="menu-note">{def.enum_notes[i]}</span></> : labels[i], search: labels[i], disabled: def.enum_disabled?.includes(ev) })
        ))]} />
    );
  }
  if (def.gui_type === 'color' || /colou?r$/.test(def.key)) {
    const v = asText(value) || '#000000';
    return <ColorPicker {...named} label={def.label || def.full_label || "Color"} value={v.slice(0, 7)} onCommit={onCommit} disabled={def.readonly}/>;
  }
  if (def.multiline || (base === 'string' && /gcode|notes|post_process/.test(def.key))) {
    return <TextBox value={asText(value)} multiline onCommit={onCommit} readOnly={def.readonly} placeholder={DESKTOP_ONLY.has(def.key) ? DESKTOP_ONLY_HINT : undefined} />;
  }
  return <TextBox value={asText(value)} label={label} numeric={NUMERIC.has(def.type)} spin={base === 'int'} def={def} onCommit={onCommit} readOnly={def.readonly} glyph={glyph} placeholder={DESKTOP_ONLY.has(def.key) ? DESKTOP_ONLY_HINT : undefined} />;
}

function OpenNumber({ def, label, value, onCommit }: { def: OptionDef; label: string; value: string; onCommit: (v: ConfigValue) => void }) {
  const named = useLabel(), errorId = useId();
  const [draft, setDraft] = useState(value);
  useEffect(() => setDraft(value), [value]);
  const issue = draft.trim() !== '' ? fieldIssue(draft, bounded(def)) : undefined;
  return <span className="control-stack">
    <EditableDropdown {...named} value={value} readOnly={def.readonly} className="text num" aria-invalid={!!issue || undefined} aria-describedby={issue ? errorId : undefined}
      onInputValueChange={setDraft} onValueChange={(next) => { setDraft(next); if (next !== value && !def.readonly && !rejectField(next, value, def, label)) onCommit(next); }}
      options={def.enum_values!.map((v, i) => ({ value: v, label: def.enum_labels?.[i] ?? v }))} />
    {issue && <span className="control-error" id={errorId} role="status">{issue.inline}</span>}
  </span>;
}


const floatStep = (n: number) => (n ? 10 ** (Math.floor(Math.log10(Math.abs(n))) - 1) : 0.1);

let pendingCommit: (() => void) | null = null;
const flushCommit = () => { const run = pendingCommit; pendingCommit = null; run?.(); };
if (typeof window !== 'undefined') for (const type of ['pointerdown', 'keydown', 'pagehide'] as const) window.addEventListener(type, flushCommit, true);
const afterPaint = (run: () => void) => { flushCommit(); pendingCommit = run; requestAnimationFrame(() => setTimeout(flushCommit, 0)); };

export function NumberBox({ labelledBy, label, value, min, max, unit, readOnly, onCommit }: { labelledBy: string; label: string; value: number; min: number; max: number; unit: string; readOnly?: boolean; onCommit(value: number): void }) {
  const def = { key: '', type: 'int', label, min, max, sidetext: unit, mode: 'simple' } as unknown as OptionDef;
  return <LabelId.Provider value={labelledBy}><span className="opt-field number-box"><TextBox numeric spin value={String(Math.round(value))} label={label} def={def} readOnly={readOnly} onCommit={(v) => onCommit(Number(v))} /></span></LabelId.Provider>;
}

const DESKTOP_ONLY_HINT = 'Runs in the desktop app';

function TextBox(props: { value: string; label?: string; multiline?: boolean; numeric?: boolean; spin?: boolean; def?: OptionDef; readOnly?: boolean; list?: string; glyph?: React.ReactNode; placeholder?: string; onCommit: (v: ConfigValue) => void }) {
  const named = useLabel();
  const errorId = useId();
  const label = props.label ?? props.def?.label ?? 'Value';
  const percentOnly = !!props.numeric && /^percents?$/.test(props.def?.type ?? '') && props.def?.sidetext?.trim() === '%';
  const shown = (v: string) => percentOnly ? v.replace(/\s*%\s*$/, '') : v;
  const [text, setText] = useState(shown(props.value));
  const issue = props.numeric && props.def && text.trim() !== '' ? fieldIssue(text, bounded(props.def)) : undefined;
  const latest = useRef(text), byKey = useRef(false);
  latest.current = text;
  useEffect(() => setText(shown(props.value)), [props.value]);
  const stored = useRef(props.value); stored.current = props.value;
  const settle = () => requestAnimationFrame(() => setTimeout(() => { latest.current = shown(stored.current); setText(shown(stored.current)); }, 0));
  const commit = () => {
    const keyed = byKey.current; byKey.current = false;
    const text = percentOnly && latest.current.trim() && !latest.current.trim().endsWith('%') ? `${latest.current.trim()}%` : latest.current;
    if (props.readOnly || text === props.value) return;
    if (props.numeric && props.def && rejectField(text, props.value, props.def, label)) { settle(); return; }
    if (keyed) { const commitText = props.onCommit; afterPaint(() => { commitText(text); settle(); }); }
    else { props.onCommit(text); settle(); }
  };
  const stepNumber = (direction: number, big = false, keyboard = false) => {
    if (props.readOnly || !props.def) return;
    const raw = latest.current.trim(), pct = percentOnly || raw.endsWith('%'), n = Number(raw.replace('%', '')) || 0;
    const step = (props.spin || props.def.type.startsWith('int') ? 1 : floatStep(n)) * (big ? 10 : 1);
    const places = Math.max(0, -Math.floor(Math.log10(step)));
    const requested = Number((n + direction * step).toFixed(places));
    const next = Math.min(props.def.max ?? Infinity, Math.max(props.def.min ?? -Infinity, requested));
    if (next !== requested) {
      const issue = fieldIssue(`${requested}${pct ? '%' : ''}`, props.def);
      toast('using', { label, value: fieldNumber(`${requested}${pct ? '%' : ''}`, props.def), detail: issue?.detail, value2: fieldNumber(`${next}${pct ? '%' : ''}`, props.def) });
    }
    const value = `${next}${pct ? '%' : ''}`;
    if (rejectField(value, props.value, props.def, label)) return;
    latest.current = shown(value); setText(shown(value));
    if (keyboard) { const commitText = props.onCommit; afterPaint(() => commitText(value)); } else props.onCommit(value);
  };
  const input = useRef<HTMLInputElement>(null), stepRef = useRef(stepNumber);
  stepRef.current = stepNumber;
  useEffect(() => {
    const el = input.current;
    if (!el || !props.numeric) return;
    const wheel = (e: WheelEvent) => { if (document.activeElement !== el || !e.deltaY) return; e.preventDefault(); stepRef.current(e.deltaY < 0 ? 1 : -1, e.shiftKey); };
    el.addEventListener('wheel', wheel, { passive: false });
    return () => el.removeEventListener('wheel', wheel);
  }, [props.numeric]);
  if (props.multiline)
    return <textarea className="text multiline" {...named} value={text} readOnly={props.readOnly} placeholder={props.placeholder} spellCheck={false} onChange={(e) => setText(e.target.value)} onBlur={commit} />;
  if (!props.numeric) return (
    <span className="control-text">
      <input {...named} className="text" value={text} readOnly={props.readOnly} placeholder={props.placeholder} list={props.list}
        onChange={(e) => setText(e.target.value)} onBlur={commit} onKeyDown={(e) => { if (e.key === 'Enter') { byKey.current = true; (e.target as HTMLInputElement).blur(); } }} />
    </span>
  );
  const side = props.def?.sidetext?.trim().replace(/^money(?=\/)/, ''), either = side?.match(/^(.+?) or %$/)?.[1], pct = text.trim().endsWith('%');
  const unit = !side ? undefined : pct ? (side === '%' || either ? undefined : side) : either ?? side;
  return (
    <span className="control-number">
      <input
        {...named}
        ref={input}
        aria-description={props.def?.sidetext}
        inputMode={props.def?.min !== undefined && props.def.min >= 0 ? 'decimal' : undefined}
        aria-invalid={!!issue || undefined}
        aria-describedby={issue ? errorId : undefined}
        className={`text num ${unit ? 'has-unit' : ''}`}
        value={text}
        readOnly={props.readOnly}
        onChange={(e) => { latest.current = e.target.value; setText(e.target.value); }}
        onBlur={commit}
        onKeyDown={(e) => {
          if (e.key === 'Enter') { byKey.current = true; (e.target as HTMLInputElement).blur(); }
          else if (e.key === 'ArrowUp' || e.key === 'ArrowDown') { e.preventDefault(); stepNumber(e.key === 'ArrowUp' ? 1 : -1, e.shiftKey, true); }
        }}
      />
      {(unit || props.glyph) && <span className="control-unit"><span aria-hidden>{unit}</span>{props.glyph}</span>}
      <span className="number-steps"><button type="button" tabIndex={-1} aria-label="Increase value" disabled={props.readOnly} onMouseDown={e => e.preventDefault()} onClick={(e) => stepNumber(1, e.shiftKey)}>+</button><button type="button" tabIndex={-1} aria-label="Decrease value" disabled={props.readOnly} onMouseDown={e => e.preventDefault()} onClick={(e) => stepNumber(-1, e.shiftKey)}>−</button></span>
      {issue && <span className="control-error" id={errorId} role="status">{issue.inline}</span>}
    </span>
  );
}
