import { currentPlate } from '../settings/common';
import { useEngineHealth } from '../flow/engine-health';
import { ExtruderSwitch, perExtruderKeys, useExtruderSelection } from '../multinozzle/NozzleControls';
import { SettingsSkeleton } from './SettingsSkeleton';
import { ExtruderRows } from '../profiles/ExtruderRows';
import { optionIndex, selectedOptionValues, type ExtruderSelection } from '../multinozzle/config';
import { PresetBar } from '../profiles/PresetBar';
import { methodModes, RETIRED_GROUPS, SurfaceSettings, TOP_METHOD_KEYS } from './SurfaceSettings';
import { methodAvailability } from '../profiles/method-context';
import { isMethodSettingSupported } from '../profiles/capabilities';
import { NozzleField } from '../profiles/SetupRows';
import { Tabs } from 'radix-ui';
import { Suspense, useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react';
import { preloadable } from '../preloadable';
import type { Config, ConfigValue, PresetType } from '../api/types';
import { isHidden, modeVisible, OPTIONS, splitKey, loadTab, useTab, type TabDesc, type TabLine, type OptionDef, type OptionMode } from '../options';
import { plateSlots } from '../printers/slot-check';
import { slotExtruder } from '../printers/feeds';
import { effectiveConfig, effectiveMode, getState, presetEdits, resetOption, setOption, setState, useStore, type State } from '../state/store';
import { belowProject, profileInfo, recordOf, selectionValues, useAskedValues, variantOf } from '../profiles/view';
import type { PrinterRecord } from '../profiles/records';
import { condition, environment, fieldDefinition, optionState } from '../visibility';
import { hiddenReason } from '../settings/common';
import { lineMode, nativeSupportCapable, tabEnvironment } from '../settings-env';
import { Field } from './Field';
import { useTabFit } from './tab-rows';
import { LabelTip } from './InfoTip';
import { displayLabel } from './fields/labels';
import { GroupHead, isGroupOpen, markOf, setGroupOpen, useSettingsGroups } from './SettingsGroup';
import { settingSources, type SettingSource } from '../profiles/bases';
import type { ObjectOverrides } from './ObjectOverrides';
import { undoable } from '../state/history';
import './settings.css';
import { slotFilaments } from '../profiles/slots';
import { unsavedRule } from '../state/changes';

const BedShapeDialog = preloadable(() => import('./BedShapeDialog'));
const nozzleCount = (s: State, printer: string) => (recordOf(s, 'machine', printer) as PrinterRecord | undefined)?.nozzles.length || 1;
const HeadPicker = preloadable(() => import('./HeadPicker'));
const HeadShapeButton = preloadable(() => import('./HeadPicker').then((m) => ({ default: m.HeadShapeButton })));
const GcodeWorkbench = preloadable(() => import('../gcode/GcodeWorkbench'), { idle: false });
const isGcode = (key: string) => /_gcode$/.test(key) && (!!OPTIONS[key]?.multiline || OPTIONS[key]?.type === 'string' || OPTIONS[key]?.type === 'strings');

const customRow = (line: TabLine) => line.label === 'Head shape' && !line.options.length;
const groupName = (s: string) => s.toLowerCase().replace(/\b(enable|enabled|use)\b/g, '').replace(/\s+/g, ' ').trim();
const groupSwitch = (g: { title: string; lines: TabLine[] }) => !g.title ? -1 : g.lines.findIndex((l) => l.options.length === 1 && !l.index_labels && !l.indexed_columns
  && OPTIONS[splitKey(l.options[0])[0]]?.type === 'bool' && groupName(l.label ?? OPTIONS[splitKey(l.options[0])[0]]?.label ?? '') === groupName(g.title));

const TAB_OF: Record<PresetType, TabDesc['tab']> = { process: 'process', filament: 'filament', machine: 'printer' };
const TYPE_NAME: Record<PresetType, string> = { process: 'Process', filament: 'Filament', machine: 'Printer' };

export interface ProfileFields { readOnlyKeys?: ReadonlySet<string>; name: string;
  state?: 'resolving' | 'ready' | 'failed'; error?: string;  printer?: string; config: Config; baseline: Config; changes: Config; mode?: OptionMode; set(key: string, value: ConfigValue): void; reset(key: string): void;
  sources?: Record<string, SettingSource>; sourceText?: Record<string, string>; resetToBase?(key: string): void;
  overridden?(key: string): boolean | undefined }
export interface Aux { label: string; icon?: string; content: React.ReactNode; modified?: boolean; initial?: boolean }
export function SettingsTab({ type, slot = 0, compact, hidePreset, editor, overrides, readOnly = false, auxiliary }: { type: PresetType; slot?: number; compact?: boolean; hidePreset?: boolean; editor?: ProfileFields;
  overrides?: ObjectOverrides | null; readOnly?: boolean; auxiliary?: Aux | Aux[] }) {
  const { desc, error } = useTab(TAB_OF[type]);
  const overriddenKey = useStore((s) => type === 'process' && !overrides ? JSON.stringify(s.objects.reduce<Record<string, string[]>>((m, o) => {
    for (const k of Object.keys(o.settings)) (m[k] ??= []).push(o.name.replace(/\.(stl|obj|3mf|step|stp)$/i, ''));
    return m; }, {})) : '{}');
  const overriddenBy = useMemo(() => JSON.parse(overriddenKey) as Record<string, string[]>, [overriddenKey]);
  const [retrying, setRetrying] = useState(false);
  const [retryError, setRetryError] = useState<string | null>(null);
  const retry = () => {
    setRetrying(true); setRetryError(null);
    void loadTab(TAB_OF[type]).catch(e => setRetryError(String(e))).finally(() => setRetrying(false));
  };
  const [bedDialog, setBedDialog] = useState(false);
  const [pageIdx, setPageIdx] = useState(0);
  const usedNozzles = useStore((s) => { const printer = s.records.printers.find((p) => p.id === s.printer), count = printer?.nozzles.length ?? 1;
    return [...new Set(plateSlots(s).map((slot) => slotExtruder(printer, count, slot - 1)).filter((e): e is number => e !== undefined))].sort().join(','); });
  const firstUsed = usedNozzles ? Number(usedNozzles.split(',')[0]) : 0;
  const [extruderSelection, setExtruderSelection] = useState<ExtruderSelection>(() => ({ extruder: firstUsed, variant: 0 }));
  useEffect(() => { if (usedNozzles && !usedNozzles.split(',').map(Number).includes(extruderSelection.extruder)) setExtruderSelection((sel) => ({ ...sel, extruder: firstUsed })); }, [usedNozzles]); // eslint-disable-line react-hooks/exhaustive-deps
  const variantKeys = perExtruderKeys(type);
  const auxes = auxiliary ? [auxiliary].flat() : [];
  const [auxIdx, setAuxIdx] = useState<number | null>(() => { const i = auxes.findIndex((a) => a.initial); return i < 0 ? null : i; });
  const aux = auxIdx !== null ? auxes[auxIdx] : undefined, auxiliaryOn = !!aux;
  const machineConfig = useStore((s) => effectiveConfig(s, 'machine'));
  const processConfig = useStore((s) => effectiveConfig(s, 'process'));
  const engine = useEngineHealth();
  const printer = useStore((s) => s.printer);
  const bedType = useStore(currentPlate);
  const projectMode = useStore(effectiveMode);
  const uiMode = editor?.mode ?? projectMode;
  const projectDirty = useStore((s) => presetEdits(s, type, slot));
  const fields = editor ?? overrides ?? undefined;
  const dirty = fields?.changes ?? projectDirty;
  const records = useStore((s) => s.records), catalogReads = useStore((s) => s.catalogReads);
  const selected = useStore((s) => selectionValues(s, type, slot)), projectConfig = selected.values;
  const answer = fields ? { state: fields.state ?? 'ready', error: fields.error } : selected, valuesReady = answer.state === 'ready';
  const config = fields?.config ?? projectConfig;
  const printerId = editor?.printer ?? (type === 'machine' && editor ? editor.name : undefined);
  const extruderPrinter = useStore((s) => s.records.printers.find((p) => p.id === (printerId ?? s.printer)));
  useExtruderSelection(type, config, extruderPrinter, extruderSelection, setExtruderSelection);
  const unedited = useMemo(() => belowProject(selected.layers), [selected.layers]);
  const sidebarProfile = useStore((s) => type === 'machine' ? s.printer : type === 'process' ? s.process : slotFilaments(s)[slot ?? 0]);
  const savedSources = useMemo(() => fields || !sidebarProfile ? undefined : settingSources(getState(), type, sidebarProfile), [fields, type, sidebarProfile, selected.layers, records]); // eslint-disable-line react-hooks/exhaustive-deps
  const sources = editor?.sources ?? savedSources;
  const system = fields?.baseline ?? unedited;
  const unsaved = useMemo(() => unsavedRule(dirty, config, system, sources), [dirty, config, system, sources]);

  const sideMachine = useAskedValues(type !== 'machine' && editor?.printer ? { type: 'machine', id: editor.printer } : null);
  const env = useMemo(() => {
    const state = getState();
    if (overrides) return environment([machineConfig, config], { printer_vendor: profileInfo(state, 'machine', state.printer).vendor,
      extruders_count: nozzleCount(state, state.printer), curr_bed_type: currentPlate(state),
      settings_scope: 'object', native_support_capable: nativeSupportCapable(state) }, uiMode);
    if (!editor) {
      const context = tabEnvironment(state, type, slot, uiMode);
      return environment([machineConfig, processConfig, selectedOptionValues(config, type, extruderSelection)], {
        printer_vendor: context.printer_vendor, extruders_count: context.extruders_count, curr_bed_type: context.curr_bed_type,
        settings_scope: 'global', native_support_capable: context.native_support_capable,
      }, uiMode);
    }
    const machine = type === 'machine' ? config : sideMachine?.state === 'ready' ? sideMachine.values : machineConfig;
    const printerId = type === 'machine' ? editor.name : editor.printer ?? state.printer;
    return environment([machine, processConfig, selectedOptionValues(config, type, extruderSelection)], {printer_vendor: profileInfo(state, 'machine', printerId).vendor,
      extruders_count: nozzleCount(state, printerId), curr_bed_type: currentPlate(state), settings_scope: 'global',
      native_support_capable: type === 'machine' ? !!variantOf(state, printerId).variant?.source.native_support_capable : nativeSupportCapable(state)}, uiMode);
  }, [config, machineConfig, processConfig, printer, bedType, uiMode, editor?.name, editor?.printer, sideMachine, !!overrides, type, slot, records, catalogReads, extruderSelection]);
  const availability = useMemo(() => methodAvailability(getState(), type === 'machine' && editor ? config : machineConfig), [machineConfig,config,type,!!editor,engine,records]);
  const states = useMemo(() => desc ? optionState(desc, env) : {}, [desc, env]);
  const fieldVisible = (k: string, line: TabLine) => {
    const key = splitKey(k)[0], rank = lineMode(line, k);
    return OPTIONS[key] && rank && !isHidden(key) && modeVisible(rank, uiMode) && (type === 'machine' || hiddenReason(k, getState(), type, slot) !== 'not applicable') && (overrides ? overrides.keys.has(key) : type !== 'process' || !TOP_METHOD_KEYS.has(key)) && isMethodSettingSupported(key,config,availability);
  };
  const focusKey = useStore((s) => (s.settingsFocus?.type === type ? s.settingsFocus.key : null));
  const pages = useMemo(() => (desc?.pages ?? []).filter((p) => !p.condition || condition(p.condition, env) !== false)
    .map((p) => ({ ...p, groups: p.groups.filter((g) => (!g.condition || condition(g.condition, env) !== false) && !(type === 'process' && !overrides && RETIRED_GROUPS.has(g.title)))
      .map((g) => ({ ...g, lines: g.lines.filter((line) =>
        line.options.some((k) => splitKey(k)[0] === focusKey) || !line.options.some((k) => states[splitKey(k)[0]]?.visible === false) && !(line.options.length && line.options.every((k) => states[splitKey(k)[0]]?.enabled === false)))
        .map((line) => ({ ...line, options: line.options.filter((k) => fieldVisible(k, line)) }))
        .filter((line, _, all) => line.options.length || (customRow(line) && all.some((l) => l.options.includes('nonplanar_head')))) }))
      .filter((g) => g.lines.length) }))
    .filter((p) => p.groups.length), [desc, env, states, uiMode, config, availability, type, focusKey, overrides?.keys]);
  const legacyKeys = useMemo(() => type !== 'process' || overrides ? [] : (desc?.pages ?? []).flatMap((p) => p.groups).filter((g) => RETIRED_GROUPS.has(g.title))
    .flatMap((g) => g.lines.filter((line) => !line.options.some((k) => states[splitKey(k)[0]]?.visible === false)).flatMap((line) => line.options.filter((k) => {
      const key = splitKey(k)[0], rank = lineMode(line, k);
      return OPTIONS[key] && rank && !isHidden(key) && modeVisible(rank, uiMode) && isMethodSettingSupported(key, config, availability);
    }).map((k) => splitKey(k)[0]))), [desc, states, uiMode, config, availability, type, !!overrides]);
  const page = pages[Math.min(pageIdx, pages.length - 1)];
  const gcodeKeys = page ? page.groups.flatMap((g) => g.lines.flatMap((l) => l.options.map((k) => splitKey(k)[0]))).filter(isGcode) : [];
  const gcodeLabels = Object.fromEntries((page?.groups ?? []).flatMap((g) => g.lines.flatMap((l) => l.options.map((k) => splitKey(k)[0]).filter(isGcode).map((k) => [k, (g.lines.length === 1 && g.title) || l.label || OPTIONS[k]?.label || k]))));
  const groups = (page?.groups ?? []).map((g) => gcodeKeys.length ? { ...g, lines: g.lines.filter((l) => !l.options.every((k) => isGcode(splitKey(k)[0]))) } : g).filter((g) => g.lines.length);
  const definitionCache = useRef(new Map<string, Partial<OptionDef>>());
  const definition = (key: string, line: TabLine) => {
    const next = {...fieldDefinition(key, env), ...line.overrides?.[key], ...key === 'nonplanar_mode' && methodModes(String(config[key] ?? 'off'), availability)}, prior = definitionCache.current.get(key);
    const same = prior && Object.keys(prior).length === Object.keys(next).length && Object.entries(next).every(([k,v]) => {
      const old = prior[k as keyof OptionDef]; return Array.isArray(v) && Array.isArray(old) ? v.length === old.length && v.every((x,i) => x === old[i]) : v === old;
    });
    if (same) return prior;
    definitionCache.current.set(key, next); return next;
  };
  useSettingsGroups();
  const groupId = (pageTitle: string, groupTitle: string) => `${TAB_OF[type]}/${pageTitle}/${groupTitle}`;
  const keysOf = (lines: TabLine[]) => lines.flatMap((l) => l.options.map((k) => splitKey(k)[0]));
  const pageMark = (p: TabDesc['pages'][number]) => markOf(p.groups.flatMap((g) => keysOf(g.lines)), unsaved);
  const focus = useStore((s) => (s.settingsFocus?.type === type ? s.settingsFocus : null));
  const strip = useRef<HTMLDivElement>(null);
  const breaks = useTabFit(strip, `${pages.map((p) => p.title).join('|')}|${auxes.map((a) => a.label).join('|')}`);
  const openByDefault = editor || overrides && !compact ? typeof window !== 'undefined' && window.innerWidth >= 1100 : uiMode === 'simple';
  const body = useRef<HTMLDivElement>(null);
  const textMeasure = useRef<CanvasRenderingContext2D | null>(null);
  const rowKeys = groups.flatMap((g) => g.lines.flatMap((l) => l.options)).join(',');
  useLayoutEffect(() => {
    const flow = editor ? body.current?.querySelector<HTMLElement>('.page-flow') : null;
    if (!flow) return;
    const probe = document.createElement('div');
    probe.className = 'opt-line';
    probe.innerHTML = '<span class="opt-label"><span class="control-check nullable-toggle"></span><button type="button" class="opt-label-text label-tip"></button></span>';
    probe.style.position = 'absolute'; probe.style.visibility = 'hidden';
    flow.append(probe);
    const textStyle = getComputedStyle(probe.querySelector('.opt-label-text')!), boxStyle = getComputedStyle(probe.querySelector('.nullable-toggle')!);
    const font = textStyle.font || `${textStyle.fontWeight} ${textStyle.fontSize} ${textStyle.fontFamily}`;
    const box = (parseFloat(boxStyle.width) || 16) + (parseFloat(boxStyle.marginLeft) || 0) + (parseFloat(boxStyle.marginRight) || 0);
    probe.remove();
    const ctx = textMeasure.current ??= document.createElement('canvas').getContext('2d');
    if (!ctx) return;
    ctx.font = font;
    const text = (t: string | undefined) => (t ? ctx.measureText(t).width : 0);
    let widest = 0;
    for (const g of groups) {
      const own = groupSwitch(g), fixed = own >= 0 && g.lines.length === 1;
      g.lines.forEach((line, row) => {
        if (row === own && !fixed) return;
        if (customRow(line)) { widest = Math.max(widest, text(line.label)); return; }
        let labels = line.index_labels, keys = labels ? line.options.flatMap((k) => labels!.map((_, i) => `${splitKey(k)[0]}#${i}`)) : line.options;
        if (line.indexed_columns) { labels = env.silent_mode ? ['Normal', 'Silent'] : undefined; keys = keys.flatMap((k) => (labels ?? ['']).map((_, i) => `${splitKey(k)[0]}#${i}`)); }
        if (keys.length > 1) {
          const key = splitKey(keys[0])[0], shared = line.label ?? (OPTIONS[key]?.full_label || OPTIONS[key]?.label);
          widest = Math.max(widest, text(shared && displayLabel(key, shared)) + 80);
          return;
        }
        const key = splitKey(keys[0])[0], def = { ...OPTIONS[key], ...definition(key, line) } as Partial<OptionDef>;
        if (def.full_width || def.multiline) return;
        const given = labels?.[0] ?? line.label, full = given ?? (def.label || def.full_label || key), shown = given === undefined ? displayLabel(key, full) : full;
        widest = Math.max(widest, text(shown) + (def.nullable ? box : 0));
      });
    }
    flow.style.setProperty('--label-w', `${Math.min(240, Math.ceil(widest) + 4)}px`);
  }, [!!editor, rowKeys, auxiliaryOn, pageIdx, !!env.silent_mode]);
  useEffect(() => {
    if (!focus || !desc) return;
    const i = pages.findIndex((p) => p.title === focus.page);
    if (i >= 0) setPageIdx(i);
    const holder = pages[i]?.groups.find((g) => g.lines.some((l) => l.options.some((k) => splitKey(k)[0] === focus.key)));
    if (holder) setGroupOpen(groupId(pages[i].title, holder.title), true);
    let tries = 0, timer: ReturnType<typeof setTimeout>;
    const reveal = () => {
      const line = body.current?.querySelector<HTMLElement>(`[data-opt="${CSS.escape(focus.key)}"]`);
      if (!line) { if (++tries < 30) timer = setTimeout(reveal, 33); return; }
      line.scrollIntoView({ block: 'center' });
      line.classList.remove('search-flash');
      void line.offsetWidth;
      line.classList.add('search-flash');
      const control = 'input:not([type=checkbox].nullable-toggle),select,textarea,button';
      (line.querySelector<HTMLElement>(`.opt-field :is(${control})`) ?? line.querySelector<HTMLElement>(control))?.focus({ preventScroll: true });
    };
    timer = setTimeout(reveal, 30);
    return () => clearTimeout(timer);
  }, [focus?.nonce, !!desc]);
  return (
    <div className={`settings-tab ${compact ? 'compact' : ''}`}>
      {bedDialog && <Suspense fallback={<div role="status">Loading bed shape…</div>}><BedShapeDialog config={config} onSet={editor?.set} close={() => setBedDialog(false)} /></Suspense>}
      {!hidePreset && <PresetBar type={type} slot={slot} />}
      <Tabs.Root className="settings-pages" value={auxiliaryOn ? `aux:${auxIdx}` : String(Math.min(pageIdx, pages.length - 1))} onValueChange={v => {const a = v.startsWith('aux:') ? Number(v.slice(4)) : null; setAuxIdx(a); if (a === null) setPageIdx(Number(v));}}><Tabs.List ref={strip} className="page-bar" data-rows={breaks.length + 1} aria-label={`${TYPE_NAME[type]} settings`}>
        {pages.map((p, i) => [breaks.includes(i) && <span key={`break-${i}`} className="tab-break" aria-hidden />, (
          <Tabs.Trigger key={p.title} value={String(i)} aria-label={p.title} className={`page-tab ${p === page && !auxiliaryOn ? 'active' : ''} ${pageMark(p) ? `modified ${pageMark(p)}` : ''}`} onClick={() => setPageIdx(i)}>
            <span className="page-tab-label">{p.title}</span>
          </Tabs.Trigger>
        )])}
        {auxes.map((a, i) => [breaks.includes(pages.length + i) && <span key={`break-aux-${i}`} className="tab-break" aria-hidden />,
          <Tabs.Trigger key={`aux-${i}`} value={`aux:${i}`} className={`page-tab ${auxIdx === i ? 'active' : ''} ${a.modified ? 'modified unsaved' : ''}`}><span className="page-tab-label">{a.label}</span></Tabs.Trigger>])}
      </Tabs.List>
      <Tabs.Content forceMount value={auxiliaryOn ? `aux:${auxIdx}` : String(Math.min(pageIdx,pages.length-1))} className="page-body" ref={body}>
        {aux ? aux.content : <><div className="page-flow">
        {type === 'machine' && !editor && <NozzleField />}
        {type === 'machine' && !overrides && page?.title === 'Extruder' && <ExtruderRows grid printerId={editor?.name} disabled={readOnly} />}
        {!valuesReady && (answer.state === 'failed'
          ? <div className="settings-load" role="alert"><span>{answer.error ?? 'Hmm, we couldn\'t resolve these settings.'}</span></div>
          : <div className="settings-load" role="status"><span>Resolving…</span></div>)}
        {valuesReady && type === 'process' && pageIdx === 0 && !overrides && <SurfaceSettings config={config} system={system} availability={availability} editor={editor} unsaved={unsaved} uiMode={uiMode} legacyKeys={legacyKeys} readOnly={readOnly}/>}
        {!desc && (error || retryError ? <div className="settings-load" role="alert"><span>Oops! Couldn't load settings</span><button type="button" className="btn" disabled={retrying} onClick={retry}>{retrying ? 'Retrying…' : 'Retry'}</button></div>
          : <SettingsSkeleton tabs={false}/>)}
        {type === 'process' && !overrides && page?.title === 'Multimaterial' && slotFilaments(getState()).length > 1 && <section className="optgroup optgroup-action"><button type="button" className="btn" onClick={() => setState({flushDialog: true})}>Flush volumes</button></section>}
        {valuesReady && groups.map((g) => {
          const single = groups.length === 1 && g.lines.length === 1 && g.lines[0].options.length === 1 && !!OPTIONS[splitKey(g.lines[0].options[0])[0]]?.multiline, id = groupId(page.title, g.title), open = !g.title || single || isGroupOpen(id, openByDefault);
          const modified = markOf(keysOf(g.lines), unsaved);
          const method = type === 'process' ? String(config.nonplanar_mode ?? '') : undefined;
          const field = (line: TabLine, keys: string[], k: string, i: number, labels: string[] | undefined, shared: boolean, tooltip: string | undefined, hide = false) => {
            const [key, column] = splitKey(k);
            const index = !overrides && !line.index_labels ? optionIndex(key, type, config, extruderSelection, column) : null;
            const indexedKey = index === null ? k : `${key}#${index}`;
            const source = sources?.[key];
            return <Field widget={type === 'machine' && key === 'printable_area' ? <button type="button" className="btn" onClick={() => setBedDialog(true)}>Set bed shape</button>
              : type === 'machine' && key === 'nonplanar_head' ? <Suspense fallback={null}><HeadPicker value={String(config[key] ?? '')} onCommit={(v) => editor ? editor.set(key, v) : v === String(system[key] ?? '') ? resetOption(type, key) : setOption(type, key, v)} /></Suspense>
              : undefined} key={`${k}:${i}`} optKey={indexedKey} type={type} value={config[key]} systemValue={system[key]} dirty={unsaved(key)}
              slot={slot} onSet={fields?.set} onReset={fields?.reset} mixed={overrides?.mixed[key]} resetTitle={overrides ? 'Reset to project' : undefined} mode={method}
              source={source} sourceText={editor?.sourceText?.[key]}
              disabled={readOnly || states[key]?.enabled === false}
              definition={editor?.readOnlyKeys?.has(key) ? { ...definition(key, line), readonly: true } : definition(key, line)}
              inheritedValue={env[key.replace(/^filament_/, '')] as ConfigValue}
              overrideChecked={editor?.overridden?.(key)}
              overrideDisabled={readOnly}
              label={labels?.[i] ?? (shared ? line.overrides?.[key]?.label ?? OPTIONS[key]?.label : line.label)}
              hideLabel={hide || g.lines.length === 1 && keys.length === 1 && !!OPTIONS[key]?.multiline && (line.label ?? OPTIONS[key]?.label ?? '').trim().toLowerCase() === g.title.trim().toLowerCase()}
              overriddenBy={overriddenBy[key]?.join(', ')} tooltip={tooltip} />;
          };
          const own = groupSwitch(g), ownLine = own >= 0 ? g.lines[own] : undefined, fixed = !!ownLine && g.lines.length === 1;
          const sub = (l: TabLine, k: string) => l.overrides?.[splitKey(k)[0]]?.label ?? OPTIONS[splitKey(k)[0]]?.label ?? '';
          const perExtruder = !overrides && g.lines.some((l) => l.options.some((k) => variantKeys.has(splitKey(k)[0])));
          const pairable = g.lines.length > 0 && own < 0 && g.lines.every((l) => l.options.length === 2 && !l.index_labels && !l.indexed_columns && !customRow(l));
          const pairs = pairable && sub(g.lines[0], g.lines[0].options[0]) && g.lines.every((l) => sub(l, l.options[0]) === sub(g.lines[0], g.lines[0].options[0]) && sub(l, l.options[1]) === sub(g.lines[0], g.lines[0].options[1]))
            ? [sub(g.lines[0], g.lines[0].options[0]), sub(g.lines[0], g.lines[0].options[1])] : null;
          return <section key={g.title} className="optgroup" data-collapsed={open || fixed ? undefined : ''} data-single={single ? '' : undefined} data-pairs={pairs ? '' : undefined} data-lone={fixed ? '' : undefined}>
            {g.title && !single && !fixed && <GroupHead id={id} title={g.title} modified={modified} open={open} onReset={readOnly ? undefined : () => undoable('Reset section', () => g.lines.flatMap((l) => l.options.map((k) => splitKey(k)[0])).filter((k) => k in dirty).forEach((k) => fields ? fields.reset(k) : resetOption(type, k, slot)))} aside={(ownLine || perExtruder) && <>{perExtruder && <ExtruderSwitch type={type} config={config} printer={extruderPrinter} selection={extruderSelection} onChange={setExtruderSelection}/>}{ownLine && field(ownLine, ownLine.options, ownLine.options[0], 0, undefined, false, ownLine.tooltip && OPTIONS[ownLine.tooltip] ? OPTIONS[ownLine.tooltip].tooltip : ownLine.tooltip, true)}</>} />}
            {open && pairs && <div className="opt-pair-head" aria-hidden><span>{pairs[0]}</span><span>{pairs[1]}</span></div>}
            {(open || fixed) && g.lines.map((line, row) => {
              if (row === own && !fixed) return null;
              let keys = line.options;
              let labels = line.index_labels;
              if (labels) keys = keys.flatMap((k) => labels!.map((_, i) => `${splitKey(k)[0]}#${i}`));
              if (line.indexed_columns) {
                labels = env.silent_mode ? ['Normal', 'Silent'] : undefined;
                keys = keys.flatMap((k) => (labels ?? ['']).map((_, i) => `${splitKey(k)[0]}#${i}`));
              }
              if (customRow(line)) return <div key={row} className="opt-line">
                <span className="opt-label"><LabelTip label={line.label} full={line.label}><strong>{line.label}</strong>{line.tooltip && <p>{line.tooltip}</p>}</LabelTip></span>
                <span className="opt-field"><Suspense fallback={null}><HeadShapeButton head={String(config.nonplanar_head ?? '')} layerHeight={Number(effectiveConfig(getState(), 'process').layer_height) || 0.2} onSaved={editor ? id => editor.set('nonplanar_head', id) : undefined} /></Suspense></span>
              </div>;
              const shared = keys.length > 1;
              const tooltip = line.tooltip && OPTIONS[line.tooltip] ? OPTIONS[line.tooltip].tooltip : line.tooltip;
              const sharedLabel = line.label ?? (OPTIONS[splitKey(keys[0])[0]]?.full_label || OPTIONS[splitKey(keys[0])[0]]?.label);
              return <div key={row} className={shared ? 'option-row' : undefined}>
                {shared && <span className="opt-label"><LabelTip label={sharedLabel && displayLabel(splitKey(keys[0])[0], sharedLabel)} full={sharedLabel}><strong>{sharedLabel}</strong>{tooltip && <p>{tooltip}</p>}</LabelTip></span>}
                {keys.map((k, i) => field(line, keys, k, i, labels, shared, tooltip))}
              </div>;
            })}
          </section>;
        })}</div>
        {gcodeKeys.length > 0 && <Suspense fallback={<div className={`gcode-bench ${groups.length ? '' : 'fill'}`} role="status" aria-label="Loading G-code editor" />}>
          <GcodeWorkbench type={type} slot={slot} keys={gcodeKeys} labels={gcodeLabels} config={config} system={system} unsaved={unsaved} editor={editor} readOnly={readOnly} fill={!groups.length} />
        </Suspense>}
        </>}
      </Tabs.Content></Tabs.Root>
    </div>
  );
}
