import { openUrlImport } from '../url-import';
import { preloadable } from '../preloadable';
import { DEMO } from '../demo/config';
import { hasWhatYouCanDo } from './what-you-can-do-page';
import { ProjectTitle } from '../projects/ProjectTitle';
import { MenuButton, type MenuEntry } from './Menu';
import { showProjects } from '../flow/project';
import { openPalette } from '../commands/registry';
import { Suspense, useEffect, useState, useSyncExternalStore, type CSSProperties } from 'react';
import { arrangeAll, canExport, STALE_RESULT_TIP, download, newProject, openFileDialog, openProjectDialog, outputKind, outsideVolumeWarning, saveProject, slice } from '../actions';
import { deleteAll, removeSelected, setState, useStore } from '../state/store';
import { canPaste, copySelection, cutSelection, pasteClipboard, selectAll } from '../edit';
import { historyVersion, redo, redoName, redoNames, redoSteps, subscribeHistory, undo, undoName, undoNames, undoSteps } from '../state/history';
import { CAMERA_VIEWS, VIEW_DIRECTIONS, cameraCommand, cameraSnapshot, subscribeCamera, perspective, autoPerspective } from '../camera';
import { startOver, useDevReset } from '../dev-reset';
import { promptDialog } from './Ask';
import { openSetup } from '../onboarding/setup-flow';
import { renameProject } from '../projects/api';
import { useProject } from '../flow/project';
import { toggleHistory } from '../projects/versions';
import { openUpdate, startUpdateChecks, useUpdate } from '../update-check';
import { APP_VERSION, reportProblem } from '../build-info';
import { Caret } from './Caret';
import { canAddPlate, duplicatePlate, exportAll, exportablePlates, sliceAll } from '../plater/plates';
import { openGcode } from '../preview/jump';
import { subscribeVisibleToolpaths, visibleToolpaths } from '../preview/visible';
import { toggleViewOption, useViewOptions } from '../plater/viewOptions';
import { toggleNonplanarView, useNonplanarView } from '../patch-preview';
import { canCalibrate, canLoadStep } from '../api/client';
import { toggleSidebar, useSidebarHidden } from '../sidebar';
import { printerReady, showSettings } from '../workspace-sheet';
import { useTier } from '../breakpoints';
import { Glyph } from './Glyph';
import { PrintingChip } from '../printers/PrintingChip';
import { sliceAndCompare } from '../compare';
import { sliceAllMethods, useMethods } from '../methods';
import { settingsChangedSinceSlice } from '../compare-settings';

type MenuItem = MenuEntry;
function Menu({title, items}: {title: string; items: MenuItem[]}) { return <MenuButton label={title} items={items} className="menu-title" />; }

const exportStl = (multi: boolean) => import('../exportStl').then((m) => m.exportStl(false, multi));

const ArchiveDialog = preloadable(() => import('./ArchiveDialog'));

const MOD = /Mac|iPhone|iPad/.test(navigator.platform) ? 'Cmd' : 'Ctrl';

export function TopBar() {
  useSyncExternalStore(subscribeCamera, cameraSnapshot);
  const view = useViewOptions();
  const nonplanarView = useNonplanarView();
  const sidebarHidden = useSidebarHidden();
  const [archive, setArchive] = useState<File[] | null>(null);
  useSyncExternalStore(subscribeHistory, historyVersion);
  const undoLabel = undoName(), redoLabel = redoName();
  const exportDrc = useStore((st) => !!st.features.export_drc);
  const generic3mf = useStore((st) => !!st.features.export_generic_3mf);
  const exportEnabled = useStore(canExport);
  const plates = useStore((s) => s.projectPlates.length > 1);
  const plateTools = useStore(canAddPlate);
  const exportAllEnabled = useStore((s) => s.projectPlates.length > 1 && s.job.state !== 'running' && s.job.state !== 'submitting' && exportablePlates(s).length > 0);
  const stale = useStore((s) => s.job.stale);
  const hasObjects = useStore((s) => s.objects.length > 0);
  const hasProjectObjects = useStore((s) => s.objects.length > 0 || s.projectPlates.some((p) => p.objects.length > 0));
  const hasSel = useStore((s) => !!s.selected);
  const hasProject = useStore((s) => !!s.project);
  const brimEarsKeys = useStore((s) => !!s.features.brim_ears);
  const hasMeshes = useStore((s) => s.objects.some((o) => !o.project?.placeholder));
  const pasteEnabled = useStore(canPaste);
  const mock = useStore((s) => s.api?.mock);
  const name = useStore((s) => (s.project?.name ?? s.objects[0]?.name)?.replace(/\.(stl|obj|3mf|drc)$/i, ''));
  const toolpaths = useSyncExternalStore(subscribeVisibleToolpaths, () => !!visibleToolpaths());
  const tab = useStore((st) => st.tab);
  const exportName = name;
  const kind = useStore(outputKind);
  const exportLabel = kind === 'gcode.3mf' ? 'Export plate sliced file' : 'Export G-code';
  const exportTip = kind === 'gcode.3mf' ? 'Export current sliced file' : 'Export current plate as G-code';
  const hasPrinters = useStore((s) => s.printerConnections.enabled);
  const fileItems: MenuItem[] = [
          { label: 'New project', shortcut: `${MOD}+N`, action: newProject, tip: 'Start a new project' },
          { label: 'Save project', shortcut: `${MOD}+S`, action: () => void saveProject(), disabled: !hasProjectObjects, tip: 'Save current project to file' },
          { label: 'Save project as…', shortcut: `${MOD}+Shift+S`, action: () => void saveProject(true), disabled: !hasProjectObjects, tip: 'Save current project as' },
          { label: 'Open project…', shortcut: `${MOD}+O`, action: openProjectDialog, tip: 'Open a project file' },
          { label: 'Recent projects', action: showProjects },
          'sep',
          { label: canLoadStep() ? 'Import 3MF/STL/STEP/OBJ…' : 'Import 3MF/STL/OBJ…', shortcut: `${MOD}+I`, action: openFileDialog, tip: 'Load a model' },
          { label: 'Import from URL…', action: () => openUrlImport() },
          { label: 'Import ZIP archive…', action: () => void import('./ArchiveDialog').then((m) => m.importZipArchive(setArchive)), tip: 'Load models contained within a ZIP archive' },
          { label: 'Open G-code…', action: () => openGcode(), tip: 'Open a G-code file' },
          { label: 'Import profiles…', action: () => setState({ presetUx: { kind: 'import' } }), tip: 'Load printer, filament and process profiles' },
          'sep',
          { label: exportLabel, shortcut: `${MOD}+G`, action: download, disabled: !exportEnabled, tip: stale ? STALE_RESULT_TIP : exportTip },
          ...(plates ? [{ label: 'Export sliced files for all plates', action: () => void exportAll(), disabled: !exportAllEnabled, tip: 'Export sliced files for all plates' }] : []),
          ...(hasPrinters ? [{ label: 'Send…', action: () => setState({ appDialog: 'send' }), disabled: !exportEnabled, tip: 'Upload the sliced file to a printer' }] : []),
          { label: 'Export all objects as one STL', action: () => void exportStl(false), disabled: !hasMeshes, tip: 'Export all objects as one STL' },
          { label: 'Export all objects as STLs', action: () => void exportStl(true), disabled: !hasMeshes, tip: 'Export all objects as STLs' },
          ...(exportDrc ? [{ label: 'Export all objects as one DRC', action: () => void import('../plater/exportModel').then((m) => m.exportModel('drc')), disabled: !hasMeshes, tip: 'Export all objects as one DRC' }] : []),
          ...(generic3mf ? [{ label: 'Export generic 3MF', action: () => void import('../plater/exportModel').then((m) => m.exportModel('generic3mf')), disabled: !hasMeshes, tip: 'Export a standard 3MF file for other slicers' }] : []),
          { label: 'Export toolpaths as OBJ', disabled: !toolpaths || tab !== 'preview', tip: 'Export toolpaths as OBJ',
            action: () => void import('../preview/objExport').then((m) => m.exportToolpathsObj(exportName || 'toolpaths')) },
          { label: 'Export preset bundle…', action: () => setState({ presetUx: { kind: 'export' } }), tip: 'Export current configuration to files' },
        ];
  const devReset = useDevReset();
  const update = useUpdate();
  useEffect(() => startUpdateChecks(), []);
  const projectId = useProject((p) => p.context.id), projectState = useProject((p) => p.value);
  const renameHere = async () => {
    const next = await promptDialog('Rename project', name ?? 'Untitled');
    if (!next || next === name) return;
    if (projectId && projectState !== 'blank') void renameProject(projectId, next); else setState({ projectName: next });
  };
  const appItems: MenuItem[] = [{label:`PancakeSlicer v${APP_VERSION}`,header:true} as MenuItem,...(update ? [{label:`Update available: v${update.version}`,action:()=>openUpdate(update)} as MenuItem] : []),{label:'About PancakeSlicer',action:()=>setState({appDialog:'about'})},{label:'Report a problem…',action:reportProblem},'sep',{label:'Add model…',action:openFileDialog},{label:'File',sub:fileItems},{label:'Settings',action:showSettings},{label:'Projects',action:showProjects},{label:'Rename project…',action:()=>void renameHere()},{label:'Setup',action:()=>openSetup('printer')},{label:'Profiles',action:()=>setState({presetUx:{kind:'presets',type:'machine'}})},'sep',...(devReset ? [{label:'Start over',action:()=>void startOver().catch(()=>{})} as MenuItem,'sep' as const] : []),{label: 'Edit', sub: [
          { label: undoLabel ? `Undo ${undoLabel}` : 'Undo', shortcut: `${MOD}+Z`, action: undo, disabled: !undoLabel },
          { label: redoLabel ? `Redo ${redoLabel}` : 'Redo', shortcut: `${MOD}+Y`, action: redo, disabled: !redoLabel },
          ...(undoLabel || redoLabel ? [{ label: 'History', sub: [
            ...redoNames().reverse().map((name, i, all): MenuItem => ({ label: `Redo ${name}`, action: () => redoSteps(all.length - i) })),
            ...(undoLabel && redoLabel ? ['sep' as const] : []),
            ...undoNames().map((name, i): MenuItem => ({ label: `Undo ${name}`, action: () => undoSteps(i + 1) })),
          ] } satisfies MenuItem] : []),
          'sep',
          { label: 'Cut', shortcut: `${MOD}+X`, action: cutSelection, disabled: !hasSel, tip: 'Cut selection to clipboard' },
          { label: 'Copy', shortcut: `${MOD}+C`, action: copySelection, disabled: !hasSel, tip: 'Copy selection to clipboard' },
          { label: 'Paste', shortcut: `${MOD}+V`, action: pasteClipboard, disabled: !pasteEnabled, tip: 'Paste clipboard' },
          { label: 'Delete selected', shortcut: 'Del', action: removeSelected, disabled: !hasSel, tip: 'Delete selected objects' },
          { label: 'Delete all', shortcut: `${MOD}+Del`, action: deleteAll, disabled: !hasObjects, tip: 'Delete all objects on the plate' },
          { label: 'Clone selected', shortcut: `${MOD}+K`, action: () => setState({ appDialog: 'clone' }), disabled: !hasSel || hasProject, tip: 'Make copies of the selected objects' },
          'sep',
          { label: 'Duplicate current plate', action: duplicatePlate, disabled: !plateTools, tip: 'Duplicate the current plate' },
          'sep',
          { label: 'Select all', shortcut: `${MOD}+A`, action: selectAll, disabled: !hasObjects, tip: 'Select all objects' },
          { label: 'Deselect all', shortcut: 'Esc', action: () => setState({ selected: null }), disabled: !hasSel, tip: 'Deselect all objects' },
          'sep',
          { label: 'Arrange', shortcut: 'A', action: arrangeAll, disabled: !hasObjects },
          'sep',
          { label: 'Search settings…', shortcut: `${MOD}+F`, action: () => openPalette('/'), tip: 'Search in all printer, filament and process settings' },
          { label: 'Preferences', shortcut: `${MOD}+P`, action: () => setState({ appDialog: 'preferences' }) },
        ]}, {label: 'View', sub: [
          ...CAMERA_VIEWS.map((label, i) => ({ label, shortcut: String(i), action: () => cameraCommand(VIEW_DIRECTIONS[i]), tip: i ? `${label} View` : label })),
          'sep',
          { label: 'Show sidebar', shortcut: `${MOD}+\\`, role: 'menuitemcheckbox', checked: !sidebarHidden, action: () => toggleSidebar() },
          'sep',
          { label: 'Use perspective view', role: 'menuitemradio', checked: perspective, action: () => cameraCommand('perspective') },
          { label: 'Use orthogonal view', role: 'menuitemradio', checked: !perspective, action: () => cameraCommand('orthogonal') },
          { label: 'Auto perspective', role: 'menuitemcheckbox', checked: autoPerspective, action: () => cameraCommand('auto') },
          'sep',
          { label: 'Show G-code window', shortcut: 'C', role: 'menuitemcheckbox', checked: view.gcodeWindow, action: () => toggleViewOption('gcodeWindow'), tip: 'Show the G-code viewer in Preview' },
          { label: 'Show 3D Navigator', role: 'menuitemcheckbox', checked: view.navigator, action: () => toggleViewOption('navigator'), tip: 'Show the 3D orientation cube in Prepare and Preview' },
          { label: 'Show gridlines', role: 'menuitemcheckbox', checked: view.gridlines, action: () => toggleViewOption('gridlines'), tip: 'Show gridlines on the build plate' },
          'sep',
          { label: 'Show labels', shortcut: brimEarsKeys ? `${MOD}+E` : 'E', role: 'menuitemcheckbox', checked: view.labels, action: () => toggleViewOption('labels'), tip: 'Show object name labels in the 3D view' },
          { label: 'Show overhang', role: 'menuitemcheckbox', checked: view.overhang, action: () => toggleViewOption('overhang'), tip: 'Highlight steep overhangs on models in the 3D view' },
          { label: 'Show non-planar surfaces', role: 'menuitemcheckbox', checked: nonplanarView, action: toggleNonplanarView, tip: "Preview which top surfaces will print curved and check their slope angles against your print head's clearance." },
          { label: 'Show edges', role: 'menuitemcheckbox', checked: view.edges, action: () => toggleViewOption('edges'), tip: "Highlight sharp model edges so contours are easier to see." },
          { label: 'Show selected outline (beta)', role: 'menuitemcheckbox', checked: view.outline, action: () => toggleViewOption('outline'), tip: 'Draw a highlight outline around the selected object' },
          { label: 'Show X-ray', role: 'menuitemcheckbox', checked: view.xray, action: () => toggleViewOption('xray'), tip: 'Make models semi-transparent so you can see inside hidden cavities and internal features.' },
          { label: 'Show center of mass', role: 'menuitemcheckbox', checked: view.com, action: () => toggleViewOption('com'), tip: 'Show each object\'s center of mass in the 3D view' },
          { label: 'Show axes', role: 'menuitemcheckbox', checked: view.axes, action: () => toggleViewOption('axes'), tip: 'Show the X, Y, and Z axes at the bed origin' },
          { label: 'Show wireframe', shortcut: `${MOD}+Shift+Enter`, role: 'menuitemcheckbox', checked: view.wireframe, action: () => toggleViewOption('wireframe'), tip: 'Show mesh wireframes in the 3D view' },
        ]}, ...(canCalibrate() ? [{label: 'Calibration', sub: [
          { label: 'Temperature', action: () => setState({ calibDialog: 'temperature' }), tip: 'Temperature Calibration' },
          { label: 'Max flowrate', action: () => setState({ calibDialog: 'max-flowrate' }), tip: 'Max flowrate' },
          { label: 'Pressure advance', action: () => setState({ calibDialog: 'pressure-advance' }), tip: 'Pressure advance' },
          { label: 'Flow ratio', action: () => setState({ calibDialog: 'flow-ratio' }), tip: 'Flow Rate Calibration' },
          { label: 'Retraction', action: () => setState({ calibDialog: 'retraction' }), tip: 'Retraction' },
          { label: 'Cornering', action: () => setState({ calibDialog: 'cornering' }), tip: 'Cornering calibration' },
          { label: 'Input shaping', sub: [
            { label: 'Input shaping frequency', action: () => setState({ calibDialog: 'input-shaping-freq' }), tip: 'Input Shaping Frequency' },
            { label: 'Input shaping damping/zeta factor', action: () => setState({ calibDialog: 'input-shaping-damp' }), tip: 'Input Shaping Damping/zeta factor' },
          ] },
          { label: 'VFA', action: () => setState({ calibDialog: 'vfa' }), tip: 'VFA' },
          { label: 'Non-planar', action: () => setState({ calibDialog: 'nonplanar' }), tip: 'Non-planar calibration' },
          'sep',
          { label: 'Calibration guide', action: () => window.open('https://www.orcaslicer.com/wiki/calibration_guide', '_blank', 'noopener'), tip: 'Calibration Guide' },
        ]} as MenuItem] : []), {label: 'Help', sub: [
          ...(hasWhatYouCanDo ? [{ label: 'What you can do', action: () => setState({ appDialog: 'what' }), tip: 'Take a visual tour of PancakeSlicer\'s features' }] : []),
          { label: 'Keyboard shortcuts', shortcut: '?', action: () => setState({ appDialog: 'shortcuts' }), tip: 'Show the list of keyboard shortcuts' },
          { label: 'Setup', action: () => openSetup('printer'), tip: 'Printer, profiles, connection and filaments' },
        ]}];
  return <header className="topbar compact-topbar">
    <div className="topbar-project-context"><MenuButton className="app-menu-button" label="Application menu" items={appItems}><img src="/logo.svg" width={20} height={20} alt="" draggable={false}/><Caret className="app-menu-caret"/></MenuButton>
    <button type="button" className="menu-title projects-button" onClick={showProjects}>Projects</button>
    <Menu title="File" items={fileItems}/>
    {update && <button type="button" className="update-pill" aria-label={`Update available: v${update.version}`} onClick={() => openUpdate(update)}><span>Update</span></button>}
    </div>
    {(DEMO || mock) && <span className="mock-badge">Demo</span>}
    {DEMO && <DemoResultNotice/>}
    {tab !== 'projects' && <TabBar/>}
    {tab === 'projects' && <div id="topbar-slot" className="topbar-slot"/>}
    {archive && <Suspense fallback={null}><ArchiveDialog files={archive} close={() => setArchive(null)} /></Suspense>}
  </header>;
}

function SideOptions({ options, value, onChange, actions = [], className = 'btn', disabled }: { options: string[]; value?: string; onChange?(v: string): void; actions?: MenuEntry[]; className?: string; disabled?: boolean }) {
  const items: MenuEntry[] = [...options.map(o => ({label: o, checked: o === value, action: () => onChange?.(o)})), ...(options.length && actions.length ? ['sep' as const] : []), ...actions];
  return <span className="side-options"><MenuButton className={`${className} slant side-drop`} label="Output options" items={items} disabled={disabled}><Caret/></MenuButton></span>;
}

export function TabBar() {
  const tab = useStore((s) => s.tab);
  const job = useStore((s) => s.job);
  const hasObjects = useStore((s) => s.objects.length > 0);
  const outside = useStore(outsideVolumeWarning);
  const busy = job.state === 'running' || job.state === 'submitting';
  const exportEnabled = useStore(canExport);
  const exportLabel = useStore(outputKind) === 'gcode.3mf' ? 'Export sliced file' : 'Export G-code';
  const printer = useStore(printerReady);
  const hasPrinters = useStore((s) => s.printerConnections.enabled);
  const plates = useStore((s) => s.projectPlates.length > 1);
  const saved = useProject((p) => p.value === 'editing' || p.value === 'saved' || p.value === 'saving');
  const [sliceMode, setSliceMode] = useState('Slice plate');
  const [exportMode, setExportMode] = useState('plate');
  const exportAllEnabled = useStore((s) => s.projectPlates.length > 1 && !busy && exportablePlates(s).length > 0);
  const tier = useTier(), wide = tier === 'wide', coarse = window.matchMedia('(pointer: coarse)').matches, folded = tier === 'narrow' || tier === 'phone' || (tier === 'medium' && coarse);
  const ready = job.state === 'done' && !job.stale && exportEnabled && !folded;
  const sliceClass = `${ready ? 'btn' : 'btn-primary'} slant${busy ? ' slicing' : ''}`, exportClass = `${ready ? 'btn-primary' : 'btn'} slant`;
  const progress = busy ? { '--p': `${Math.round(job.pct)}%` } as CSSProperties : undefined;
  const allSlice = wide && plates && sliceMode === 'Slice all';
  const methods = useMethods(), changed = useStore(settingsChangedSinceSlice) && !methods;
  const compareSlice = () => { sliceAndCompare(slice); setState({ tab: 'preview' }); };
  const sliceMethods: MenuEntry = {label: 'Slice all methods', action: () => void sliceAllMethods(), disabled: !hasObjects || !printer || busy || !!outside};
  const allExport = wide && plates && exportMode === 'all';
  const more: MenuEntry[] = [
    ...(folded ? [sliceMethods] : []),
    ...(folded && changed ? [{label: 'Slice and compare', action: compareSlice, disabled: !printer || busy || !!outside, tip: 'Slice with your updated settings and compare side by side with the previous slice'}] : []),
    ...(folded ? [{label: exportLabel, action: download, disabled: !exportEnabled, tip: job.stale ? STALE_RESULT_TIP : 'Slice the plate first'}] : []),
    ...(!wide && hasPrinters ? [{label: 'Send to printer', action: () => setState({appDialog: 'send'}), disabled: !exportEnabled}] : []),
    ...(tier === 'phone' && saved ? [{label: 'Version history', action: toggleHistory} satisfies MenuEntry] : []),
    ...(!wide && plates ? [{label: 'Slice all plates', action: () => void sliceAll(), disabled: !printer || busy}, {label: 'Export all plates', action: () => void exportAll(), disabled: !exportAllEnabled}] : []),
  ];
  return (
    <div className="tabbar">
      <div className="seg slant-seg view-switch" role="group" aria-label="View">
        <button type="button" aria-label="Prepare" aria-pressed={tab === 'prepare'} className="view-tab" onClick={() => setState({ tab: 'prepare' })}>
          <span>Prepare</span>
        </button>
        <button type="button" aria-label="Preview" aria-pressed={tab === 'preview'} className="view-tab" onClick={() => setState({ tab: 'preview' })}>
          <span>Preview</span>
        </button>
        <button type="button" aria-label="Device" aria-pressed={tab === 'device'} className="view-tab" onClick={() => setState({ tab: 'device' })}>
          <span>Device</span>
        </button>
      </div>
      <div className="title-zone"><ProjectTitle/></div>
      <div className="tabbar-actions">
        <PrintingChip/>
        {allSlice ? (
          <button className={sliceClass} style={progress} disabled={!printer || busy} onClick={() => void sliceAll()}>Slice all</button>
        ) : (
          <button className={sliceClass} style={progress} disabled={!hasObjects || !printer || busy || !!outside} onClick={() => void slice()}>
            {tier === 'phone' ? 'Slice' : 'Slice plate'}
          </button>
        )}
        {!folded && <SideOptions className={ready ? 'btn' : 'btn-primary'} options={wide && plates ? ['Slice all', 'Slice plate'] : []} value={sliceMode} onChange={setSliceMode}
          actions={[sliceMethods]} disabled={!printer || busy || (!plates && !hasObjects)} />}
        {!allSlice && !folded && changed && <button className="btn slant" disabled={!printer || busy || !!outside} onClick={compareSlice}>Slice and compare</button>}
        {!folded && <span className="export-actions">{allExport ? (
          <button className={exportClass} disabled={!exportAllEnabled} onClick={() => void exportAll()}>Export all sliced plates</button>
        ) : (
          <button className={exportClass} disabled={!exportEnabled} onClick={download}>
            {exportLabel}
          </button>
        )}
        {wide && plates && <SideOptions options={[exportLabel, 'Export all sliced plates']} value={allExport ? 'Export all sliced plates' : exportLabel}
          onChange={(v) => setExportMode(v === 'Export all sliced plates' ? 'all' : 'plate')} />}
        {wide && hasPrinters && (
          <button className="btn slant" disabled={!exportEnabled} onClick={() => setState({ appDialog: 'send' })}>
            Send
          </button>
        )}
        </span>}
        {more.length > 0 && <MenuButton className="btn slant output-overflow" label="More output actions" items={more}><Glyph name="more"/></MenuButton>}
      </div>
    </div>
  );
}

function DemoResultNotice() {
  const done = useStore(s => s.job.state === 'done' && s.tab !== 'projects');
  return done ? <span className="demo-result-notice" role="status">Demo: showing pre-sliced result</span> : null;
}
