import { ProfileLibraryNotice } from './ProfileLibraryNotice';
import { openSlideOutEditor } from '../navigation';
import { preloadable } from '../preloadable';
import { nativeSurfaceSummary } from '../preview/explain-record';
import { Fragment, Suspense, useEffect, useLayoutEffect, useRef, useState, type ReactNode } from 'react';
import { railSlotsHeight, rememberRailLayout } from './rail-layout';
import { bootShape } from '../boot-shape';
import { HeldText } from './Skeleton';
import { railWidthNow } from '../sidebar';
import { SidebarSkeleton } from './SidebarSkeleton';
import { toggleSidebar } from '../sidebar';
import { SettingsSkeleton } from './SettingsSkeleton';
import { feedOf, slotLabels, unitsOf } from '../printers/feeds';
import { materialNote, reportText, trayLibraryFilament, usePrinterSlots } from '../printers/slots';
import type { PresetType } from '../api/types';
import { effectiveMode, getState, printerPreset, setState, toast, useStore, type State } from '../state/store';
import { profileInfo, recordOf, selectionValues } from '../profiles/view';
import type { PrinterRecord } from '../profiles/records';
import { valueAt } from '../multinozzle/config';
import { printerShortName } from '../profiles/Pickers';
import { filamentSlots as capacity } from '../profiles/machines';
import { addFilament, FilamentMenu, PlateTypeField, removeSlot } from '../profiles/SetupRows';
import { PresetCombo, requestPreset } from './PresetCombo';
import { ChangesMark, PresetActions, PrinterChangesMark } from '../profiles/PresetBar';
import { MenuButton, type MenuEntry } from './Menu';
import { Caret } from './Caret';
import { flushSavedPreferences, preferenceText, savePreferences, usePreference } from '../saved-preferences';
import { printerCover } from '../profiles/metadata';
import './settings.css';
import { Glyph, type GlyphName } from './Glyph';
import { AutoInputs } from '../profiles/AutoInputs';
import { PrinterLive } from '../printers/PrinterLive';
import { slotFilaments } from '../profiles/slots';
import { useMachine } from '../flow/fsm';
import { project } from '../flow/project';
import { urlProject } from '../flow/routes';
import { noticeOf, projectSlots } from '../flow/project-slots';
import { ProjectNoticeRow } from './ProjectNotice';
import { RecordProblem } from '../profiles/RecordProblem';
const SettingsTab = preloadable(() => import('./SettingsTab').then(m => ({default: m.SettingsTab})));

function RailSection({title, summary, meta, actions, className = '', children}: {title: string; summary?: ReactNode; meta?: ReactNode; actions?: ReactNode; className?: string; children: ReactNode}) {
  const folded = (usePreference('orca.rail_folded') ? preferenceText('orca.rail_folded')! : '').split(',').includes(title);
  const [opening, setOpening] = useState(false);
  const fold = () => {
    const rest = (preferenceText('orca.rail_folded') ?? '').split(',').filter(t => t && t !== title);
    savePreferences({'orca.rail_folded': (folded ? rest : [...rest, title]).join(',')});
    void flushSavedPreferences().catch(() => {});
    if (folded) { setOpening(true); setTimeout(() => setOpening(false), 150); }
  };
  return <section className={`rail-section ${className}${folded ? ' folded' : ''}${opening ? ' opening' : ''}`} aria-label={title}>
    <header className="rail-head">
      <h2 className="rail-title"><button type="button" className="rail-fold" aria-expanded={!folded} onClick={fold}>
        <Caret dir={folded ? 'right' : 'down'}/><span className="rail-title-text">{title}</span>{folded && summary && <span className="rail-summary">{summary}</span>}
      </button></h2>
      {meta}{actions && <div className="rail-actions">{actions}</div>}
    </header>
    {!folded && children}
  </section>;
}
const presetName = (type: PresetType, id: string | undefined) => (id ? profileInfo(getState(), type, id).name : '');
const printerRecord = (s: State) => recordOf(s, 'machine', s.printer) as PrinterRecord | undefined;
const hasPrinter = (s: State) => { const r = printerRecord(s); return !!r && !r.archived; };
function PrinterSummary() {
  const info = useStore(s => profileInfo(s, 'machine', s.printer)), machine = useStore(s => selectionValues(s, 'machine'));
  return <>{info.name ? printerShortName(info.name, machine.values, info.vendor ?? '') : ''}</>;
}
function RailAction({glyph, label, disabled, onClick}: {glyph: GlyphName; label: string; disabled?: boolean; onClick(): void}) {
  return <button type="button" className="icon-btn rail-act" aria-label={label} disabled={disabled} onClick={onClick}><Glyph name={glyph}/></button>;
}
const edit = (type: PresetType, name?: string) => name && openSlideOutEditor(type, name);

function PrinterSection() {
  const machine = useStore(printerPreset), name = useStore(s => s.printer), ready = useStore(hasPrinter);
  const [failedCover, setFailedCover] = useState<string>();
  const cover = ready && machine ? printerCover(machine.vendor, machine.config, machine.model) : undefined;
  return <RailSection title="Printer" summary={<PrinterSummary/>} meta={ready && <PrinterChangesMark/>} actions={<>
    <RailAction glyph="settings" label="Edit printer" disabled={!ready} onClick={() => edit('machine', name)}/>
    {ready && <SlotMenu/>}
    <RailAction glyph="back" label="Hide sidebar (Ctrl+\\)" onClick={() => toggleSidebar(true)}/>
  </>}>
    <div className="rail-row rail-printer">
      <div className="rail-lead">{cover && failedCover !== cover ? <img className="rail-cover" src={cover} alt="" loading="lazy" width={40} height={40} onError={() => setFailedCover(cover)}/> : <span className="rail-cover" aria-hidden="true"/>}<PrinterLive/></div>
      <div className="rail-printer-controls"><PresetCombo type="machine"/>{ready && <PlateTypeField bare/>}<RecordProblem kind="machine" id={name}/></div>
    </div>
    {ready && <SlotRows/>}
  </RailSection>;
}
const noticeActions = { use: () => projectSlots.send({ type: 'USE' }), keep: () => projectSlots.send({ type: 'KEEP' }), dismiss: () => projectSlots.send({ type: 'DISMISS' }) };
function ProjectSlotsChoice() {
  const { value, context } = useMachine(projectSlots);
  const notice = noticeOf(value, context.offer, context.error);
  return notice ? <ProjectNoticeRow notice={notice} actions={noticeActions}/> : null;
}

function SlotRows() {
  const printer = useStore(printerRecord), slots = (printer?.slots ?? []).map((slot) => slot.filament);
  const labels = slotLabels(printer, printer?.model, slots.length);
  const connected = useStore(s => s.printerConnections.connections.some(c => c.printer === s.printer && c.kind === 'bambu-lan'));
  const read = usePrinterSlots(connected ? printer?.id : undefined), reports = read.slots;
  const linking = useStore(s => { const st = s.printerConnections.status[s.printer]; return !st || st.link === 'connecting'; });
  const listed = useStore(s => s.printerConnections.enabled !== null);
  const boot = bootShape(printer?.id), bootSlots = boot?.slots.length === slots.length ? boot.slots : undefined;
  const awaiting = (listed ? connected : !!boot?.connected) && (linking || (!reports && !read.error));
  const PENDING = '\u0000';
  const types = useStore(s => slotFilaments(s).map((f, i) => { const v = selectionValues(s, 'filament', i); return f && v.state === 'ready' ? String(valueAt(v.values.filament_type, 0) ?? '') : PENDING; }).join('\n')).split('\n').map((t) => t === PENDING ? undefined : t);
  const names = useStore(s => slots.map((f) => f ? profileInfo(s, 'filament', f).name : '').join('\n')).split('\n');
  const reportOf = (i: number) => reports?.online ? reports.slots.find((r) => r.slot === printer?.slots[i]?.tray) : undefined;
  const noteLine = (i: number) => { const n = bootSlots?.[i]?.note; return !!n && <HeldText className="rail-slot-mismatch" text={materialNote(n.tray, names[i] || n.name, n.type) ?? ''}/>; };
  const heldNote = (i: number) => !!slots[i] && types[i] === undefined && noteLine(i);
  const inTray = useStore(s => slots.map((f, i) => f || !printer ? '' : trayLibraryFilament(s, printer.id, reportOf(i)) ?? '').join('\n')).split('\n');
  const report = (i: number) => { const r = reportOf(i), note = slots[i] ? materialNote(r?.tray.type ?? '', names[i], types[i]) : null;
    return r && <>{reportText(r)}{inTray[i] && <>{' '}<button type="button" className="link rail-slot-use" onClick={() => void requestPreset('filament', i, inTray[i])}>Use it</button></>}
      {note ? <span className="rail-slot-mismatch" role="note">{note}</span> : heldNote(i)}</>; };
  const unknown = !listed || awaiting || (!!reports?.online && types.some((t, i) => !!slots[i] && t === undefined));
  const group = useRef<HTMLDivElement>(null), [kept, setKept] = useState<number | null>(null);
  useLayoutEffect(() => {
    const aside = group.current?.closest('aside');
    setKept(unknown && aside && printer ? railSlotsHeight(printer.id, aside.getBoundingClientRect().width, slots.length) : null);
  }, [unknown, printer?.id, slots.length]); // eslint-disable-line react-hooks/exhaustive-deps
  const tail = (i: number) => <span className="rail-tail"><ChangesMark type="filament" slot={i} compact/><RailAction glyph="settings" label={`Edit ${labels[i].label} filament`} disabled={!slots[i]} onClick={() => edit('filament', slots[i])}/></span>;
  const heading = (i: number) => labels[i].group && labels[i].group !== labels[i - 1]?.group && <div className="rail-slot-group">{labels[i].group}</div>;
  return <div ref={group} className="rail-slots" role="group" aria-label="Slots" style={kept ? { minHeight: kept } : undefined} data-kept={kept ? '' : undefined}><ProjectSlotsChoice/>{slots.map((_, i) => <Fragment key={i}>
    {heading(i)}
    <div className="rail-row rail-slot"><PresetCombo type="filament" slot={i}/>{tail(i)}
      {reportOf(i) && <span className="rail-slot-report">{report(i)}</span>}
      {awaiting && (bootSlots ? bootSlots[i]?.report : true) && <span className="rail-slot-report" aria-hidden="true"><span className="skel-line" style={{ display: 'inline-block', width: '60%' }}>{'\u00a0'}</span>{noteLine(i)}</span>}</div>
  </Fragment>)}</div>;
}
function SlotMenu() {
  const machine = useStore(printerPreset), record = useStore(printerRecord), slots = record?.slots.length ?? 0, feed = feedOf(record?.model);
  const connected = useStore(s => s.printerConnections.connections.some(c => c.printer === s.printer && c.kind === 'bambu-lan'));
  const max = capacity(machine), units = feed && record ? unitsOf(feed, record) : [];
  const attach = (next: { nozzle: number }[]) => record && feed && void import('../printers/device-slots').then(({ setUnits }) => setUnits(record.id, next, feed.unit)).catch((e: Error) => toast('sidebar_failed', { detail: e.message }));
  const nozzleFor = () => feed?.external === 'perNozzle' ? [...Array((feed.nozzles ?? ['']).length).keys()].sort((a, b) => units.filter(u => u.nozzle === a).length - units.filter(u => u.nozzle === b).length)[0] : 0;
  const items: MenuEntry[] = !feed ? [
    ...(max > 1 && slots < max ? [{label: 'Add slot', action: addFilament}] : []),
    ...(slots > 1 ? [{label: `Remove slot ${slots}`, action: () => removeSlot(slots - 1)}] : []),
  ] : connected ? [] : [
    ...(units.length < feed.maxUnits ? [{label: `Attach ${feed.unit} (${feed.slots} slots)`, action: () => attach([...units, {nozzle: nozzleFor()}])}] : []),
    ...(units.length > 0 ? [{label: units.length > 1 ? `Detach ${feed.unit} ${units.length}` : `Detach ${feed.unit}`, action: () => attach(units.slice(0, -1))}] : []),
  ];
  return <FilamentMenu extra={items}/>;
}
const DETAIL = [['simple', 'Simple'], ['advanced', 'Advanced'], ['develop', 'Expert']] as const;
type Detail = (typeof DETAIL)[number][0];
function ViewChoices() {
  const mode = useStore(effectiveMode), detail: Detail = mode === 'developer' ? 'develop' : mode;
  const setDetail = (id: Detail) => { savePreferences({'orca.settings_detail': id, 'orca.developer_mode': '0'}); setState({uiMode: id, developerMode: false}); };
  const items: MenuEntry[] = DETAIL.map(([id, text]) => ({label: text, role: 'menuitemradio', checked: id === detail, action: () => setDetail(id)}));
  const label = DETAIL.find(([id]) => id === detail)?.[1] ?? 'Simple';
  return <>
    <span className="rail-menus"><MenuButton className="rail-menu" label={`Settings detail: ${label}`} items={items}>{label}<Caret/></MenuButton></span>
    <MenuButton className="icon-btn rail-act rail-menus-folded" label="View options" items={[{label: 'Detail', header: true}, ...items]}><Glyph name="view"/></MenuButton>
  </>;
}
function SliceSummary() {
  const summary = useStore(s => s.tab !== 'preview' && s.job.state === 'done' && !s.job.stale && s.job.status?.record ? nativeSurfaceSummary(s.job.status.record) : null);
  return summary ? <div className="engine-line" role="status">{summary}</div> : null;
}
export function Sidebar() {
  const ready = useStore(hasPrinter), process = useStore(s => s.process), processName = useStore(s => presetName('process', s.process));
  const loading = useStore(s => !s.api || s.profileLibraryLoad.state === 'loading');
  const opening = useMachine(project, (p) => p.value === 'blank' || p.value === 'opening') && !!urlProject();
  const aside = useRef<HTMLElement>(null);
  useEffect(() => {
    const el = aside.current;
    if (!el) return;
    let timer = 0;
    const save = () => { clearTimeout(timer); timer = window.setTimeout(() => { if (el.querySelectorAll(':scope > section.rail-section').length > 1) rememberRailLayout(el, railWidthNow()); }, 500); };
    const sizes = new ResizeObserver(save);
    const watch = () => { sizes.disconnect(); sizes.observe(el); el.querySelectorAll(':scope > section.rail-section').forEach((section) => sizes.observe(section)); save(); };
    const sections = new MutationObserver(watch);
    sections.observe(el, { childList: true }); watch();
    return () => { clearTimeout(timer); sizes.disconnect(); sections.disconnect(); };
  }, [ready, loading, opening]);
  if ((!ready && loading) || opening) return <SidebarSkeleton/>;
  return <aside ref={aside} className="sidebar scroll-fade">
    <ProfileLibraryNotice/>
    <PrinterSection/>
    {ready && <>
      <RailSection title="Print settings" summary={processName} className="process-section" meta={<ChangesMark type="process"/>} actions={<><ViewChoices/><RailAction glyph="settings" label="Edit print settings" disabled={!process} onClick={() => edit('process', process)}/><PresetActions type="process"/></>}>
        <div className="rail-row rail-row-wide"><PresetCombo type="process"/></div>
        <AutoInputs/>
        <div className="process-panel"><Suspense fallback={<SettingsSkeleton/>}><SettingsTab type="process" compact hidePreset/></Suspense></div>
      </RailSection>
      <SliceSummary/>
    </>}
  </aside>;
}
