import { recordOf } from './view';
import { speedModelOf } from './speed-rules';
import { openLibrary, openSlideOutEditor } from '../navigation';
import { focusSetting, locate } from './settings-focus';
import { SaveMenu } from './SaveMenu';
import { Dropdown } from '../components/Dropdown';
import { modelLabel } from './metadata';
import { requestAddSettings } from '../components/add-settings-request';
import { findProfile, profileKey } from './identity';
import { useEffect, useId, useMemo, useRef, useState } from 'react';
import type { PresetType } from '../api/types';
import { MenuButton } from '../components/Menu';
import { Caret } from '../components/Caret';
import { Popover } from 'radix-ui';
import { OPTIONS, splitKey, TABS } from '../options';
import { getState, resolvePreset, setState, useStore, type State } from '../state/store';
import { useMachine } from '../flow/fsm';
import { display, isMetaKey } from './format';
import { Glyph } from '../components/Glyph';
import { undoable } from '../state/history';
import { PresetPicker } from './Pickers';
import { ProcessPresetPicker } from '../components/ProcessPresetPicker';
import '../components/process-settings.css';
import { revert, revertAll } from './stack';
import { selectChanges, type SettingChange as Change } from '../state/changes';
import { useChanges } from '../state/useChanges';
export { useChanges } from '../state/useChanges';
import { reviewProjectProfileChanges } from './project-draft';
import { toast } from '../state/store';
import { editing } from './editing';
import { profileSaveFor } from '../flow/profile-save';
import { defaultChoice, formulaText, newProfileName, newProfileNameError, saveAsNewProfile, saveToProfile, savesInPlace, scaleContext, solveFactor, valueSources, type NewKind, type ValueChoice, type ValueSource } from './save-to-profile';
import { labelOf } from './PrintOverrides';
import { Sep } from '../separator';
import { slotFilaments } from './slots';
import { exportProfiles } from './library-io';
import { InLayer } from '../components/PageLayer';

async function exportCurrent(type: PresetType, slot: number) {
    await exportProfiles({ kind: 'current', type, slot });
}

interface MenuItem { label: string; run(): void; disabled?: boolean; output?: boolean }

function PresetMenu({ label, items }: { label: string; items: MenuItem[] }) {
  return <MenuButton className="icon-btn preset-more" label={label} items={items.map(it => ({label: it.label, action: it.run, disabled: it.disabled, output: it.output}))}>⋯</MenuButton>;

}

const TYPE_NAME: Record<PresetType, string> = { process: 'Process', filament: 'Filament', machine: 'Printer' };

const where = (c: Change) => c.scope === 'project' ? c.owner.name : `${c.scope === 'object' ? 'Object' : c.scope === 'part' ? 'Part' : 'Range'}: ${c.owner.name}`;

const label = (c: Change) => c.key === 'nonplanar_mode' ? 'Slicing method' : OPTIONS[c.key] && TABS.process?.pages.some((p) => p.groups.some((g) => g.lines.some((l) => l.options.some((k) => splitKey(k)[0] === c.key)))) ? labelOf(c.key) : OPTIONS[c.key]?.label || c.key;

function ValueChoices({ c, src, choice, becomes, onChoose }: { c: Change; src: ValueSource; choice: ValueChoice; becomes: string; onChoose(next: ValueChoice): void }) {
  const now = display(c.key, c.now);
  const options: [ValueChoice, string][] = src.source === 'formula'
    ? [['formula', 'Update formula'], ['fix', `Fix at ${now}`], ['project', 'Only this project']]
    : src.source === 'calibration' || src.source === 'filament' ? [['calibration', `Save for ${src.printer}`], ['filament', `Save to ${src.filament}`], ['project', 'Only this project']]
    : [['printer', `Save to ${src.printer}`], ['project', 'Only this project']];
  const text = choice === 'formula' ? (becomes ? `updates formula to ${becomes}` : 'updates formula') : choice === 'fix' ? `fixed at ${now}`
    : choice === 'printer' || choice === 'calibration' ? `saves for ${src.printer}` : choice === 'filament' ? `saves to ${src.filament}` : 'only this project';
  return <MenuButton className="pf-change-choice" label={`Save ${label(c)}: ${text}`} items={options.map(([id, t]) => ({ label: t, checked: choice === id, role: 'menuitemradio', action: () => onChoose(id) }))}>
    <span aria-hidden>↳</span> {text} <Caret dir="down"/></MenuButton>;
}

function Changes({ type, slot, list, onJump, sources = {}, choices = {}, onChoose }: { type: PresetType; slot: number; list: Change[]; onJump(): void; sources?: Record<string, ValueSource>; choices?: Record<string, ValueChoice>; onChoose?(key: string, next: ValueChoice): void }) {
  const ctxKey = useStore((s) => { const c = scaleContext(s); return `${c.nozzle},${c.layerHeight}`; }), [nozzle, layerHeight] = ctxKey.split(',').map(Number), ctx = { nozzle, layerHeight };
  const focus = (c: Change) => {
    onJump();
    const s = getState();
    if (c.owner.object !== undefined) {
      const object = s.objects.find((o) => o.id === c.owner.object);
      if (!object) return;
      setState({ selected: { object: object.id, instance: object.instances[0]?.id ?? 0, volume: c.owner.volume, range: c.owner.range } });
      requestAddSettings('open');
      setTimeout(() => focusSetting('process', c.key, label(c)), 120);
      return;
    }
    if (type === 'process') return focusSetting(type, c.key, label(c));
    openSlideOutEditor(type, (type === 'machine' ? s.printer : slotFilaments(s)[slot]) ?? '', c.key);
  };
  const rows = list.map((c) => ({ c, at: locate(type, c.key) })).sort((x, y) => x.at.pi - y.at.pi || x.at.gi - y.at.gi);
  const scoped = new Set(list.map(where)).size > 1;
  return <div className="pf-change-scroll" role="list">{rows.flatMap(({ c }) => {
    const out = [];
    const name = label(c), was = display(c.key, c.was), now = display(c.key, c.now), scope = scoped ? where(c) : '';
    const src = c.scope === 'project' ? sources[c.key] : undefined, choice = src && (choices[c.key] ?? defaultChoice(src));
    const factor = src?.row && choice === 'formula' ? solveFactor(src.row, c.now, ctx) : undefined;
    const becomes = src?.row && factor !== undefined ? formulaText({ ...src.row, factor }) : '';
    out.push(<div key={`${c.scope}:${c.owner.object ?? ''}:${c.owner.volume ?? ''}:${c.owner.range ?? ''}:${c.key}`} className="pf-change" role="listitem">
      <button type="button" className="pf-change-line" aria-label={`${name}${scope ? `, ${scope}` : ''}: ${was} to ${now}`} data-full-text={`${name}${scope ? ` (${scope})` : ''}: ${was} → ${now}`} onClick={() => focus(c)}>
        <span className="pf-change-name">{name}</span><span className="pf-change-was">{was}</span><span className="pf-change-to" aria-hidden>→</span><span className="pf-change-now">{now}{scope && <span className="pf-change-scope"><Sep/>{scope}</span>}</span>
      </button>
      <button type="button" className="reset-icon" aria-label={`Reset ${name}`} onClick={() => revert(c)}><Glyph name="undo" size={14}/></button>
    </div>);
    if (src && choice && onChoose) out.push(<ValueChoices key={`choice:${c.key}`} c={c} src={src} choice={choice} becomes={becomes} onChoose={(next) => onChoose(c.key, next)}/>);
    return out;
  })}</div>;
}

const REVIEW = 'pf-review';
let pending: { type: PresetType; slot: number; at: number } | null = null;

export function reviewChanges(type: PresetType, slot = 0) {
  pending = { type, slot, at: Date.now() };
  if (type === 'process') {
  } else {
    const d = getState().dialog;
    if (d?.type !== type || d.slot !== slot) setState({ dialog: { type, slot } });
  }
  window.dispatchEvent(new CustomEvent(REVIEW, { detail: { type, slot } }));
}

function ChangeGroupBody({type, slot, list, project, title, onJump}: {type: PresetType; slot: number; list: Change[]; project: number; title?: string; onJump(): void}) {
  const sources = useStore((s) => JSON.stringify(valueSources(s, type, slot)));
  const sourced = useMemo(() => JSON.parse(sources) as Record<string, ValueSource>, [sources]);
  const [choices, setChoices] = useState<Record<string, ValueChoice>>({});
  if (!list.length) return null;
  return <section className="pf-change-group" aria-label={title ?? 'Changes'}>
    {title && <h3 className="pf-change-group-title">{title}</h3>}
    <Changes type={type} slot={slot} list={list} onJump={onJump} sources={sourced} choices={choices} onChoose={type === 'filament' ? undefined : (k, v) => setChoices((c) => ({...c, [k]: v}))}/>
    <SaveChanges type={type} slot={slot} project={project} list={list} sources={sourced} choices={choices} revertAll={() => undoable('Revert all changes', () => list.forEach(revert))}/>
  </section>;
}
const countText = (n: number) => `${n} change${n === 1 ? '' : 's'}`;

function ChangesPopover({review, total, children, compact, label}: {review: {type: PresetType; slot: number}[]; total: number; children(onJump: () => void): React.ReactNode; compact?: boolean; label?: string}) {
  const [open, setOpen] = useState(false);
  const jumped = useRef(false);
  const key = JSON.stringify(review);
  useEffect(() => {
    const take = () => {
      if (pending && review.some((r) => r.type === pending!.type && r.slot === pending!.slot) && Date.now() - pending.at < 3000) { pending = null; setOpen(true); }
    };
    take();
    window.addEventListener(REVIEW, take);
    return () => window.removeEventListener(REVIEW, take);
  }, [key]); // eslint-disable-line react-hooks/exhaustive-deps
  useEffect(() => { if (!total) setOpen(false); }, [total]);
  if (!total) return null;
  const onJump = () => {jumped.current = true; setOpen(false);};
  return <Popover.Root open={open} onOpenChange={setOpen}><Popover.Trigger asChild><button type="button" className={`link pf-changes-trigger${compact ? ' compact' : ''}`} aria-expanded={open} aria-label={compact ? `${countText(total)}${label ? ` to ${label}` : ''}` : undefined}>{compact ? total : countText(total)}</button></Popover.Trigger><InLayer portal={Popover.Portal}><Popover.Content className="orca-menu pf-changes-pop" role="dialog" aria-label="Changes" collisionPadding={12} side="bottom" sideOffset={4} align="start" onCloseAutoFocus={(e) => {if (jumped.current) {jumped.current = false; e.preventDefault();}}}
    onFocusOutside={(e) => {if ((e.target as Element | null)?.closest?.('[role=menu]')) e.preventDefault();}}
    onPointerDownOutside={(e) => {if ((e.target as Element | null)?.closest?.('[role=menu]')) e.preventDefault();}}>
    {children(onJump)}
  </Popover.Content></InLayer></Popover.Root>;
}

function SaveChanges({type, slot, project, list, sources, choices, revertAll}: {type: PresetType; slot: number; project: number; list: Change[]; sources: Record<string, ValueSource>; choices: Record<string, ValueChoice>; revertAll(): void}) {
  const profile = useStore((s) => findProfile(s, type, (type === 'machine' ? s.printer : type === 'process' ? s.process : slotFilaments(s)[slot]) ?? ''));
  const m = useMemo(() => profileSaveFor(JSON.stringify([type, profile ? profileKey(profile) : `${type}:${slot}`])), [type, slot, profile && profileKey(profile)]), snap = useMachine(m);
  const projectList = list.filter((c) => c.scope === 'project' && c.owner.plate === undefined);
  const savable = projectList.some((c) => !sources[c.key] || (choices[c.key] ?? defaultChoice(sources[c.key])) !== 'project');
  const owned = useStore((s) => savesInPlace(s, type, slot)), inPlace = owned && savable;
  const target = useStore((s) => findProfile(s, type, (type === 'machine' ? s.printer : type === 'process' ? s.process : slotFilaments(s)[slot]) ?? '')?.name ?? 'profile');
  const dynamic = useStore((s) => type === 'process' && !!speedModelOf(findProfile(s, type, s.process)));
  const suggested = useStore((s) => newProfileName(s, type, slot));
  const [name, setName] = useState(suggested);
  useEffect(() => setName(suggested), [suggested]);
  const nameError = useStore((s) => newProfileNameError(s, type, name));
  const nameErrorId = useId();
  const saving = snap.value === 'saving', error = snap.value === 'failed' ? snap.context.error : null;
  const [last, setLast] = useState<NewKind | 'in-place' | 'printer'>('in-place');
  const every = (choice: ValueChoice) => Object.fromEntries(projectList.map((c) => [c.key, choice]));
  const printerModel = useStore((s) => { const r = resolvePreset(s, 'machine', s.printer); return modelLabel(r.config, r.base?.vendor ?? '') || r.name || 'this printer'; });
  const run = (how: NewKind | 'in-place' | 'printer') => {
    if (!m.can({type: 'SAVE'})) return;
    setLast(how);
    m.send({type: 'SAVE'});
    const request = m.get().context.request;
    const chosen = type !== 'filament' ? choices : how === 'printer' ? every('calibration') : every('filament');
    (how === 'in-place' || how === 'printer' ? saveToProfile(type, slot, chosen) : saveAsNewProfile(type, slot, name, how, chosen))
      .then(() => m.send({type: 'SAVED', request}), (e) => m.send({type: 'FAIL', request, error: e instanceof Error ? e.message : String(e)}));
  };
  const [naming, setNaming] = useState<NewKind | null>(null);
  const busy = (how: NewKind | 'in-place' | 'printer') => saving && last === how && <span className="pf-spin" aria-hidden/>;
  const retry = (how: NewKind | 'in-place' | 'printer', text: string) => error && last === how ? 'Retry' : text;
  return <>
    {error && <p className="pf-save-error" role="alert">{error}</p>}
    {project > 0 && naming && dynamic && <Dropdown className="combo pf-save-kind" aria-label="New profile kind" value={naming} options={[{value: 'dynamic', label: 'Dynamic profile'}, {value: 'regular', label: 'Regular profile'}]} onValueChange={(v) => setNaming(v as NewKind)}/>}
    {project > 0 && naming && <label className="pf-save-name"><span>{naming === 'dynamic' ? 'New dynamic profile' : naming === 'regular' ? 'New regular profile' : 'New profile'}</span>
      <input className="text" autoFocus value={name} disabled={saving} aria-invalid={!!nameError} aria-describedby={nameError ? nameErrorId : undefined}
        onChange={(e) => {setName(e.target.value); if (error) m.send({type: 'DISMISS'});}} onKeyDown={(e) => {if (e.key === 'Enter' && !nameError) run(naming); else if (e.key === 'Escape') {e.stopPropagation(); setNaming(null);}}}/></label>}
    {project > 0 && naming && nameError && <p className="control-error" id={nameErrorId} role="status">{nameError}</p>}
    {snap.value === 'conflict' && <div className="pf-editor-error" role="alert">{target} was saved in another tab or device.
      <span className="pf-conflict-actions"><button type="button" className="btn" onClick={() => m.send({type: 'KEEP'})}>Keep my changes</button>
      <button type="button" className="btn-ghost" onClick={() => {revertAll(); m.send({type: 'DISCARD'});}}>Discard them</button></span></div>}
    <div className="pf-changes-foot"><button type="button" className="link pf-revert-all" disabled={saving} onClick={revertAll}>Revert all</button>
      {project > 0 && (naming ? <>
        <button type="button" className="btn-ghost" disabled={saving} onClick={() => setNaming(null)}>Cancel</button>
        <button type="button" className="btn primary" disabled={saving || !!nameError} aria-busy={saving} onClick={() => run(naming)}>{busy(naming)}{retry(naming, 'Create')}</button>
      </> : <>
        <SaveMenu name={target} busy={saving} disabled={saving} targets={{ ...(inPlace ? { profile: () => run('in-place') } : {}),
          ...(inPlace && type === 'filament' ? { forPrinter: { model: printerModel, run: () => run('printer') } } : {}), asNew: () => setNaming(dynamic ? 'dynamic' : 'same') }}/>
      </>)}</div>
  </>;
}

function usePresetActions(type: PresetType, slot: number) {
  const list = useChanges(type, slot).filter(listed);
  const project = list.filter((c) => c.scope === 'project' && c.owner.plate === undefined).length;
  const m = editing(type, slot);
  const state = useMachine(m, (x) => x.value);
  useEffect(() => m.send({ type: 'EDITS', n: project }), [m, project]);
  const presetName = useStore((s) => (type === 'machine' ? s.printer : type === 'process' ? s.process : slotFilaments(s)[slot]));
  const isUser = useStore((s) => !!recordOf(s, type, presetName ?? ''));

  const onDelete = () => {
    const key = presetName ?? '';
    if (recordOf(getState(), type, key)) void import('./LibraryPage').then((m) => m.removePreset(type, key));
  };
  const save=()=>{try {reviewProjectProfileChanges(type,slot);}catch(error){toast('presetbar_failed', { detail: error instanceof Error ? error.message : String(error) });}};
  return { list, project, state, presetName, isUser, onDelete, save };
}

export function PresetActions({ type, slot = 0 }: { type: PresetType; slot?: number }) {
  const { project, presetName, isUser, onDelete, save } = usePresetActions(type, slot);
  return (
    <PresetMenu label={`${TYPE_NAME[type]} preset actions`} items={[
      { label: 'Review project changes in profile editor', run: save, disabled: !project && isUser },

      { label: 'Revert all', run: () => revertAll(type, slot), disabled: !project },
      { label: 'Compare', run: () => openLibrary(type, { name: presetName, compare: '' }) },
      { label: 'Edit profile', run: () => presetName && openSlideOutEditor(type, presetName) },
      { label: 'Export', output: true, run: () => void exportCurrent(type, slot).catch((e) => toast('presetbar_failed_2', { detail: e instanceof Error ? e.message : String(e) })) },
      { label: 'Delete', run: onDelete, disabled: !isUser },
    ]} />
  );
}

export function ChangesMark({ type, slot = 0, compact }: { type: PresetType; slot?: number; compact?: boolean }) {
  const {list,project,presetName}=usePresetActions(type,slot);
  const label = useStore((s) => resolvePreset(s, type, presetName ?? '').name);
  return <ChangesPopover review={[{type, slot}]} total={list.length} compact={compact} label={label}>{(onJump) => <ChangeGroupBody type={type} slot={slot} list={list} project={project} onJump={onJump}/>}</ChangesPopover>;
}

const listed = (c: Change) => !isMetaKey(c.key) && c.owner.object === undefined;
const filamentGroups = (slots: readonly string[]) => [...slots.reduce((m, key, i) => key ? m.set(key, [...(m.get(key) ?? []), i]) : m, new Map<string, number[]>()).values()];
function SlotGroup({slots, onJump}: {slots: number[]; onJump(): void}) {
  const {list, project} = usePresetActions('filament', slots[0]);
  const name = useStore((s) => resolvePreset(s, 'filament', slotFilaments(s)[slots[0]]).name ?? '');
  const where = slots.length > 1 ? `Slots ${slots.map((i) => i + 1).join(', ')}` : `Slot ${slots[0] + 1}`;
  return <ChangeGroupBody type="filament" slot={slots[0]} list={list} project={project} title={`${where}: ${name}`} onJump={onJump}/>;
}
function PrinterGroup({onJump}: {onJump(): void}) {
  const {list, project} = usePresetActions('machine', 0);
  const name = useStore((s) => resolvePreset(s, 'machine', s.printer).name ?? 'Printer');
  return <ChangeGroupBody type="machine" slot={0} list={list} project={project} title={name} onJump={onJump}/>;
}
let countedFrom: unknown[] = [], counted = 0;
function printerChangeCount(s: State): number {
  const inputs = [s.dirty, s.filamentEdits, s.projectPresets, s.projectPlates, s.plateIndex, s.records, s.resolved, s.printer, slotFilaments(s), s.profileSnapshots];
  if (inputs.length === countedFrom.length && inputs.every((v, i) => v === countedFrom[i])) return counted;
  countedFrom = inputs;
  counted = [selectChanges(s, 'machine', 0), ...filamentGroups(slotFilaments(s)).map((g) => selectChanges(s, 'filament', g[0]))].reduce((n, l) => n + l.filter(listed).length, 0);
  return counted;
}
export function PrinterChangesMark() {
  const groups = JSON.parse(useStore((s) => JSON.stringify(filamentGroups(slotFilaments(s))))) as number[][];
  const total = useStore(printerChangeCount);
  const review = [{type: 'machine' as const, slot: 0}, ...groups.flat().map((slot) => ({type: 'filament' as const, slot}))];
  return <ChangesPopover review={review} total={total}>{(onJump) => <div className="pf-change-groups"><PrinterGroup onJump={onJump}/>{groups.map((g) => <SlotGroup key={g.join(',')} slots={g} onJump={onJump}/>)}</div>}</ChangesPopover>;
}

export function PresetBar({ type, slot = 0 }: { type: PresetType; slot?: number }) {
  const { presetName } = usePresetActions(type, slot);
  return (
    <>
      <div className="tab-preset-row">
        <div className="profile-choice-row">{type === 'process' ? <ProcessPresetPicker/> : <PresetPicker type={type} slot={slot}/>}</div>
        <div className="profile-action-row"><button type="button" className="btn profile-edit-action" onClick={() => presetName && openSlideOutEditor(type, presetName)}>Edit</button>
        <ChangesMark type={type} slot={slot}/><PresetActions type={type} slot={slot}/></div>
      </div>
    </>
  );
}
