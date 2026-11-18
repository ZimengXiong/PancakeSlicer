import { sameValue } from '../state/value-equality';
import { Dropdown } from '../components/Dropdown';
import { Field } from '../components/Field';
import { Glyph } from '../components/Glyph';
import { LabelTip } from '../components/InfoTip';
import { MenuButton, type MenuEntry } from '../components/Menu';
import { useTabLoaded } from '../components/PresetDialogs';
import { OPTIONS, splitKey, TABS } from '../options';
import type { Config, ConfigValue } from '../api/types';
import { toolValue } from '../config-values';
import { nozzleElements, valueAt, valuesOf } from '../multinozzle/config';
import type { PrinterRecord } from './records';
import { SEP } from '../separator';
import { display } from './format';

export const tabKeys = (tab: 'process' | 'filament') => [...new Set((TABS[tab]?.pages ?? []).flatMap((p) => p.groups.flatMap((g) => g.lines.flatMap((l) => l.options.map((k) => splitKey(k)[0])))))]
  .filter((k) => OPTIONS[k]?.label && k !== 'layer_height' && !OPTIONS[k]?.multiline);
const NAMES: Record<string, string> = { nozzle_temperature: 'Nozzle temp', nozzle_temperature_initial_layer: 'Nozzle temp, first layer',
  hot_plate_temp: 'Smooth plate temp', hot_plate_temp_initial_layer: 'Smooth plate temp, first layer', textured_plate_temp: 'Textured plate temp',
  textured_plate_temp_initial_layer: 'Textured plate temp, first layer', cool_plate_temp: 'Cool plate temp', cool_plate_temp_initial_layer: 'Cool plate temp, first layer',
  eng_plate_temp: 'Engineering plate temp', eng_plate_temp_initial_layer: 'Engineering plate temp, first layer', fan_min_speed: 'Fan min', fan_max_speed: 'Fan max',
  filament_flow_ratio: 'Flow ratio', enable_pressure_advance: 'Pressure advance on', filament_max_volumetric_speed: 'Max flow',
  filament_retraction_length: 'Retraction length', filament_retraction_speed: 'Retraction speed', filament_deretraction_speed: 'Deretraction speed', filament_z_hop: 'Z hop' };
export const settingName = (key: string) => NAMES[key] || OPTIONS[key]?.full_label || OPTIONS[key]?.label || key;
const repeated = new WeakMap<object, Set<string>>();
function repeatedNames(): Set<string> {
  const desc = TABS.filament;
  if (!desc) return new Set();
  let out = repeated.get(desc);
  if (!out) {
    const seen = new Map<string, number>();
    for (const k of tabKeys('filament')) seen.set(settingName(k), (seen.get(settingName(k)) ?? 0) + 1);
    repeated.set(desc, (out = new Set([...seen].filter(([, n]) => n > 1).map(([name]) => name))));
  }
  return out;
}
export const labelOf = (key: string, tab: 'process' | 'filament' = 'process') => {
  if (NAMES[key]) return settingName(key);
  if (tab === 'filament') {
    const name = settingName(key);
    if (!repeatedNames().has(name)) return name;
    const line = TABS.filament?.pages.flatMap((p) => p.groups).flatMap((g) => g.lines).find((l) => l.options.some((k) => splitKey(k)[0] === key));
    return line?.label ? `${line.label}${SEP}${line.overrides?.[key]?.label ?? OPTIONS[key]?.label ?? name}` : name;
  }
  const group = TABS[tab]?.pages.flatMap((p) => p.groups).find((g) => g.lines.some((l) => l.options.some((k) => splitKey(k)[0] === key)))?.title;
  const own = OPTIONS[key]?.label || key;
  if (!group) return OPTIONS[key]?.full_label || own;
  const g = group.toLowerCase(), o = own.toLowerCase();
  const words = (t: string) => new Set(t.split(/[^a-z]+/).filter((w) => w.length > 2).map((w) => w.replace(/s$/, '')));
  const shared = [...words(o)].some((w) => words(g).has(w));
  return o.includes(g) || shared && !g.includes(o) ? own : g.includes(o) ? group : `${group}${SEP}${own}`;
};

export function overrideText(key: string, value: ConfigValue, config: Config, printer: PrinterRecord | undefined, type: 'process' | 'filament' = 'process'): string {
  if (!OPTIONS[key]?.type.endsWith('s')) return display(key, value);
  const parts = nozzleElements(key, type, config, printer).map((e) => ({ label: e.label, text: display(key, valueAt(value, e.index)) }));
  return parts.every((p) => p.text === parts[0].text) ? parts[0].text : parts.map((p) => `${p.label} ${p.text}`).join(', ');
}

export function OverrideList({ tab, value, defaults = {}, printer, readOnly, onChange, rowMenu, allow = () => true, fixed }: { tab: 'process' | 'filament'; value: Config; defaults?: Config;
  printer?: PrinterRecord; readOnly?: boolean; onChange(next: Config): void; rowMenu?(key: string): MenuEntry[]; allow?(key: string): boolean;
  fixed?(key: string): string | undefined }) {
  useTabLoaded(tab);
  const keys = Object.keys(value).sort((a, b) => labelOf(a, tab).localeCompare(labelOf(b, tab)));
  const remove = (key: string) => { const next = { ...value }; delete next[key]; onChange(next); };
  const set = (key: string, next: ConfigValue) => {
    if (defaults[key] !== undefined && sameValue(key, next, defaults[key])) { remove(key); return; }
    onChange({ ...value, [key]: typeof value[key] === 'string' && Array.isArray(next) ? toolValue(key, next) : next }); };
  const add = (key: string) => onChange({ ...value, [key]: defaults[key] ?? '' });
  const type = tab === 'process' ? 'process' : 'filament';
  return <section className="print-overrides">
    {keys.length > 0 && <div className="po-rows">{keys.map((k) => {
      const menu = rowMenu ? rowMenu(k) : [], label = labelOf(k, tab), def = OPTIONS[k], why = fixed?.(k), locked = readOnly || !!why;
      const extra = why ? <span className="po-fixed muted">{why}</span> : !readOnly && (menu.length
        ? <MenuButton className="icon-btn row-remove" label={`${label} actions`} items={[...menu, { label: k in defaults ? 'Use default' : 'Remove', action: () => remove(k) }]}><Glyph name="more" size={14}/></MenuButton>
        : <button type="button" className="reset-icon row-remove" aria-label={k in defaults ? 'Use default' : 'Remove'} onClick={() => remove(k)}><Glyph name="close" size={14}/></button>);
      const vector = !!def?.type.endsWith('s'), elements = vector ? nozzleElements(k, type, defaults, printer) : [{ index: 0 }];
      const size = Math.max(valuesOf(value[k]).length, valuesOf(defaults[k]).length);
      const shown = vector && size > valuesOf(value[k]).length ? Array.from({ length: size }, (_, i) => valueAt(value[k], i)) : value[k];
      const field = (e: { index: number; label?: string }, i: number) => <Field key={e.index} optKey={vector ? `${k}#${e.index}` : k} type={type} value={shown} systemValue={defaults[k]} dirty={false}
        label={e.label ?? label} disabled={locked} onSet={set} onReset={remove} extra={i === 0 ? extra : undefined}/>;
      return elements.length > 1
        ? <div key={k} className="option-row po-row"><span className="opt-label"><LabelTip label={label} full={label}><strong>{label}</strong>{def?.tooltip && <p>{def.tooltip}</p>}</LabelTip></span>{elements.map(field)}</div>
        : <div key={k} className="po-row">{field(elements[0], 0)}</div>;
    })}</div>}
    {!readOnly && <Dropdown className="combo" aria-label="Add a setting" value="" placeholder="Add a setting" onValueChange={add}
      options={tabKeys(tab).filter((k) => !(k in value) && allow(k)).sort((a, b) => labelOf(a, tab).localeCompare(labelOf(b, tab))).map((k) => ({ value: k, label: labelOf(k, tab) }))}/>}
  </section>;
}

export function PrintOverrides({ value, defaults, printer, readOnly, onChange }: { value: Config; defaults?: Config; printer?: PrinterRecord; readOnly?: boolean; onChange(next: Config): void }) {
  return <OverrideList tab="process" value={value} defaults={defaults} printer={printer} readOnly={readOnly} onChange={onChange}/>;
}
