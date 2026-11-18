import { useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import { useAddSettingsRequest } from './add-settings-request';
import type { Config, ConfigValue } from '../api/types';
import objectKeys from '../object_options.json';
import { toolValue } from '../config-values';
import { display } from '../profiles/format';
import { effectiveConfig, getState, removeObjectOptions, removeVolumeOptions, setObjectOption, setVolumeOption, useStore,
  isSelected, type LayerRange, type PlateObject, type State } from '../state/store';
import { undoable } from '../state/history';
import { selectionValues } from '../profiles/view';
import { addRangeAfter, canAddAfter, deleteRange, editRange, rangeLabel, removeRangeOptions, setRangeOption } from '../plater/ranges';
import { Field } from './Field';
import { Glyph } from './Glyph';
import { OPTIONS } from '../options';
import { SettingsTab, type ProfileFields } from './SettingsTab';
import { objectLabels } from '../plater/object-names';
import './object-overrides.css';
import { sameValue } from '../state/value-equality';

const REGION_KEYS = new Set(objectKeys.region);
const OBJECT_KEYS = new Set([...objectKeys.region, ...objectKeys.object]);
const editable = (o: PlateObject) => !o.project;

export interface ObjectOverrides extends ProfileFields { keys: Set<string>; mixed: Record<string, string>;
  target: { kind: 'objects'; ids: number[] } | { kind: 'part'; object: number; volume: number } | { kind: 'range'; object: number; range: LayerRange } }


function overridesOf(s: State, project: Config): ObjectOverrides | null {
  const sel = s.selected, object = sel ? s.objects.find((o) => o.id === sel.object) : undefined;
  const single = (kind: 'part' | 'range', settings: Config, name: string, set: (k: string, v: ConfigValue) => void, remove: (keys: string[]) => void, target: ObjectOverrides['target']): ObjectOverrides => {
    const baseline = { ...project, ...object!.settings };
    return { name, keys: REGION_KEYS, mixed: {}, target, config: { ...baseline, ...settings }, baseline, changes: settings,
      set: (k, v) => undoable(kind === 'part' ? 'Edit part settings' : 'Edit range settings', () => sameValue(k, v, baseline[k]) ? remove([k]) : set(k, v), `${name}:${k}`),
      reset: (k) => undoable('Reset to object', () => remove([k])) };
  };
  if (object && editable(object) && sel?.range !== undefined) {
    const range = object.ranges?.find((r) => r.id === sel.range);
    if (range) return single('range', range.settings, rangeLabel(range), (k, v) => setRangeOption(object.id, range.id, k, v), (keys) => removeRangeOptions(object.id, range.id, keys), { kind: 'range', object: object.id, range });
  }
  if (object && editable(object) && sel?.volume !== undefined) {
    const volume = object.volumes?.find((v) => v.id === sel.volume);
    if (volume && (volume.type === 'part' || volume.type === 'modifier'))
      return single('part', volume.settings ?? {}, volume.name, (k, v) => setVolumeOption(object.id, volume.id, k, v), (keys) => removeVolumeOptions(object.id, volume.id, keys), { kind: 'part', object: object.id, volume: volume.id });
  }
  const objects = s.objects.filter((o) => editable(o) && o.instances.some((i) => isSelected(sel, o.id, i.id)));
  if (!objects.length) return null;
  const ids = objects.map((o) => o.id);
  const config: Config = { ...project }, changes: Config = {}, mixed: Record<string, string> = {};
  const touched = new Set(objects.flatMap((o) => Object.keys(o.settings)));
  for (const k of touched) {
    const values = objects.map((o) => o.settings[k] ?? project[k]);
    config[k] = values[0];
    changes[k] = values[0];
    const distinct = [...new Map(values.filter((v) => v !== undefined).map((v) => [toolValue(k, v), v])).values()];
    if (distinct.length > 1) mixed[k] = distinct.slice(0, 3).map((v) => display(k, v)).join(', ') + (distinct.length > 3 ? ', …' : '');
  }
  return {
    name: objects.length === 1 ? objectLabels(s.objects).get(objects[0].id)! : `${objects.length} objects`, keys: OBJECT_KEYS, mixed, target: { kind: 'objects', ids },
    config, baseline: project, changes,
    set: (k, v) => undoable('Edit object settings', () => objects.forEach((o) => sameValue(k, v, project[k]) ? removeObjectOptions(o.id, [k]) : setObjectOption(o.id, k, v)), `objects:${ids}:${k}`),
    reset: (k) => undoable('Reset to project', () => objects.forEach((o) => removeObjectOptions(o.id, [k]))),
  };
}

export function useObjectOverrides(): ObjectOverrides | null {
  const objects = useStore((s) => s.objects), selected = useStore((s) => s.selected);
  const project = useStore((s) => effectiveConfig(s, 'process'));
  const answer = { state: useStore((s) => selectionValues(s, 'process').state), error: useStore((s) => selectionValues(s, 'process').error) };
  return useMemo(() => { const o = overridesOf(getState(), project); return o && { ...o, state: answer.state, error: answer.error }; }, [objects, selected, project, answer.state, answer.error]);
}

export function RangeEditor({ objectId, range }: { objectId: number; range: LayerRange }) {
  const object = useStore((s) => s.objects.find((o) => o.id === objectId));
  const field = (label: string, value: number, commit: (v: number) => boolean) => (
    <label className="form-row">
      <span>{label}</span>
      <input key={`${range.id}:${value}`} className="text" type="number" step={0.01} defaultValue={value} aria-label={label}
        onBlur={(e) => { const v = Number(e.target.value); if (v !== value && !(Number.isFinite(v) && commit(v))) e.target.value = String(value); }}
        onKeyDown={(e) => { if (e.key === 'Enter') (e.target as HTMLInputElement).blur(); }} />
    </label>
  );
  return (
    <div className="range-editor">
      {field('Min Z (mm)', range.z[0], (v) => editRange(objectId, range.id, { z: [v, range.z[1]] }))}
      {field('Max Z (mm)', range.z[1], (v) => editRange(objectId, range.id, { z: [range.z[0], v] }))}
      {field('Layer height (mm)', range.layer_height, (v) => editRange(objectId, range.id, { layer_height: v }))}
      <span className="range-tools">
        <button type="button" className="btn" disabled={!object || !canAddAfter(object, range.id)} onClick={() => addRangeAfter(objectId, range.id)}>Add range</button>
        <button type="button" className="btn-ghost" onClick={() => deleteRange(objectId, range.id)}>Delete range</button>
      </span>
    </div>
  );
}


export default function ObjectSettings() {
  const overrides = useObjectOverrides();
  const [open, setOpen] = useState(false);
  useAddSettingsRequest(!!overrides, (mode) => setOpen((o) => mode === 'open' || !o));
  if (!overrides) return null;
  const keys = Object.keys(overrides.changes).filter((k) => OPTIONS[k]);
  return <section className="ol-settings" aria-label={`${overrides.name} settings`}>
    <header className="ol-settings-head"><span className="ol-settings-name">{overrides.name}</span>
      <button type="button" className="icon-btn rail-act" aria-label={`Add settings to ${overrides.name}`} aria-expanded={open} onClick={() => setOpen((o) => !o)}><Glyph name="plus" size={14}/></button></header>
    {overrides.target.kind === 'range' && <RangeEditor objectId={overrides.target.object} range={overrides.target.range} />}
    {keys.map((k) => <Field key={k} optKey={k} type="process" value={overrides.config[k]} systemValue={overrides.baseline[k]} dirty
      onSet={overrides.set} onReset={overrides.reset} mixed={overrides.mixed[k]} resetTitle="Project"/>)}
    {open && <ObjectSettingsPanel overrides={overrides} onClose={() => setOpen(false)}/>}
  </section>;
}

function ObjectSettingsPanel({ overrides, onClose }: { overrides: ObjectOverrides; onClose(): void }) {
  const n = Object.keys(overrides.changes).filter((k) => OPTIONS[k]).length;
  const card = document.querySelector<HTMLElement>('.objects-panel'), panel = useRef<HTMLElement>(null);
  const [box, setBox] = useState<{ width: number; maxHeight: number }>();
  useLayoutEffect(() => {
    const area = card?.closest<HTMLElement>('.canvas-area');
    if (!card || !area) return;
    const place = () => {
      const c = card.getBoundingClientRect(), a = area.getBoundingClientRect(), inset = c.left - a.left;
      setBox({ width: Math.max(c.width, Math.min(720, a.right - c.left - inset)), maxHeight: Math.max(160, a.bottom - c.bottom - inset - 6) });
    };
    place();
    const observer = new ResizeObserver(place);
    observer.observe(card); observer.observe(area);
    return () => observer.disconnect();
  }, [card]);
  useEffect(() => {
    panel.current?.focus({ preventScroll: true });
    const key = (e: KeyboardEvent) => { if (e.key === 'Escape' && !e.defaultPrevented) { e.preventDefault(); onClose(); } };
    document.addEventListener('keydown', key);
    return () => document.removeEventListener('keydown', key);
  }, [onClose]);
  if (!card) return null;
  return createPortal(<section ref={panel} tabIndex={-1} className="object-settings-panel" role="dialog" aria-label={`${overrides.name} settings`} style={box}>
    <header className="object-settings-head"><h2>{overrides.name}{n > 0 && <span className="object-settings-count">{n} change{n === 1 ? '' : 's'}</span>}</h2>
      <button type="button" className="icon-btn" aria-label="Close" onClick={onClose}><Glyph name="close" size={14}/></button></header>
    <div className="pf-full-editor object-settings-body"><SettingsTab type="process" overrides={overrides} hidePreset/></div>
  </section>, card);
}
