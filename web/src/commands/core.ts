import { reportProblem } from '../build-info';
import { openUrlImport } from '../url-import';
import { savePreferences } from '../saved-preferences';
import { addInstance, arrangeAll, canBoolean, canExport, cancelSlice, download, fillBed, openFileDialog, outputKind,
  removeInstance, setInstanceCount, slice, STALE_RESULT_TIP, toggleBoolean, toggleSvg, toggleText, toolbarOperation, outsideVolumeWarning } from '../actions';
import { canDrop, canPaste, centreSelection, copySelection, cutSelection, dropSelection, mirrorSelection, pasteClipboard, selectAll, startRename } from '../edit';
import { deleteAll, getState, removeSelected, setState, togglePrintable, type Gizmo, type State } from '../state/store';
import { redo, redoName, undo, undoName } from '../state/history';
import { CAMERA_VIEWS, VIEW_DIRECTIONS, cameraCommand } from '../camera';
import { toggleViewOption, viewOptions, type ViewOptions } from '../plater/viewOptions';
import { toggleNonplanarView } from '../patch-preview';
import { toggleSidebar } from '../sidebar';
import { canPaint, togglePaint, type PaintKind } from '../plater/paint';
import { cutBlocked, toggleCut } from '../plater/cut';
import { addPlate, canAddPlate, duplicatePlate, exportAll, exportablePlates, sliceAll } from '../plater/plates';
import { assembly, setAssembly } from '../plater/assembly';
import { openGcode } from '../preview/jump';
import { canCalibrate } from '../api/client';
import { setThemePref } from '../theme';
import { canEditLayers } from '../components/LayerHeight';
import { registerCommand, type Command } from './registry';

const s = getState;
const objects = () => s().objects.length > 0 || 'Add a model first';
const selection = () => !!s().selected || 'Select an object first';
const editable = () => {
  const st = s(), o = st.objects.find((x) => x.id === st.selected?.object);
  return !st.selected ? 'Select an object first' : st.selected.all ? 'Select one object' : o?.project?.placeholder ? 'Locked in an imported 3MF project' : true;
};
const noProject = (f: () => boolean | string) => () => s().project ? 'Not in an opened project' : f();
const exportable = () => {
  const st = s();
  if (!st.objects.length) return 'Add a model first';
  if (st.job.stale) return STALE_RESULT_TIP;
  return canExport(st) || outsideVolumeWarning(st) || 'Slice first';
};
const busy = (st: State) => st.job.state === 'running' || st.job.state === 'submitting';
const gizmo = (g: Gizmo) => () => setState((st) => ({ gizmo: st.gizmo === g ? 'none' : g, tab: 'prepare' }));
const printers = () => s().printerConnections.enabled || (s().printerConnections.enabled === false ? 'Printer network connections are disabled on this server' : 'Connecting to printers');
const app = (d: NonNullable<State['appDialog']>) => () => setState({ appDialog: d });

const VIEW_TOGGLES: [keyof ViewOptions, string, string?][] = [
  ['labels', 'Show labels'], ['overhang', 'Show overhang'], ['gridlines', 'Show gridlines'], ['navigator', 'Show 3D navigator'],
  ['outline', 'Show selected outline'], ['xray', 'Show X-ray'], ['com', 'Show center of mass'], ['axes', 'Show axes'],
  ['wireframe', 'Show wireframe', 'Mod+Shift+Enter'], ['gcodeWindow', 'Show G-code window', 'C'],
];
const PAINTS: [PaintKind, string, string][] = [['support', 'Paint support', 'Shift+L'], ['seam', 'Paint seam', 'P'], ['fuzzy', 'Paint fuzzy skin', 'H'], ['color', 'Paint color', 'N']];
const CALIB: [NonNullable<State['calibDialog']>, string][] = [
  ['temperature', 'Temperature'], ['max-flowrate', 'Max flowrate'], ['pressure-advance', 'Pressure advance'], ['flow-ratio', 'Flow ratio'],
  ['retraction', 'Retraction'], ['cornering', 'Cornering'], ['input-shaping-freq', 'Input shaping frequency'], ['input-shaping-damp', 'Input shaping damping'],
  ['vfa', 'VFA'], ['nonplanar', 'Non-planar'],
];

export const CORE: Command[] = [
  { id: 'file.importurl', title: 'Import from URL…', group: 'File', keywords: 'link download printables thingiverse makerworld github', run: () => openUrlImport() },
  { id: 'file.import', title: 'Import models…', group: 'File', keys: 'Mod+I', keywords: 'add open stl 3mf obj step load', run: openFileDialog, boost: 60 },
  { id: 'file.gcode', title: 'Open G-code…', group: 'File', run: () => openGcode() },
  { id: 'file.configs', title: 'Import profiles…', group: 'File', keywords: 'presets bundle configs', run: () => setState({ presetUx: { kind: 'import' } }) },
  { id: 'file.export', get title() { return exportTitle(); }, group: 'File', keys: 'Mod+G', keywords: 'download save sliced file gcode 3mf', enabled: exportable, run: download, boost: 80 },
  { id: 'file.send', title: 'Print — send to printer…', group: 'File', keywords: 'send upload printer bambu octoprint prusalink', enabled: () => printers() === true ? exportable() : printers(), run: app('send'), boost: 80 },
  { id: 'file.exportall', title: 'Export all plates', group: 'File', enabled: () => s().projectPlates.length > 1 ? !busy(s()) && exportablePlates(s()).length > 0 || 'Slice the plates first' : 'One plate only', run: () => void exportAll() },
  { id: 'file.stl', title: 'Export all objects as one STL…', group: 'File', keywords: 'mesh', enabled: objects, run: () => void import('../exportStl').then((m) => m.exportStl(false, false)) },
  { id: 'file.stls', title: 'Export all objects as STLs…', group: 'File', keywords: 'mesh', enabled: objects, run: () => void import('../exportStl').then((m) => m.exportStl(false, true)) },
  { id: 'file.bundle', title: 'Export profiles…', group: 'File', keywords: 'configs presets bundle', run: () => setState({ presetUx: { kind: 'export' } }) },

  { id: 'edit.undo', title: 'Undo', group: 'Edit', keys: 'Mod+Z', enabled: () => !!undoName() || 'Nothing to undo', run: undo },
  { id: 'edit.redo', title: 'Redo', group: 'Edit', keys: 'Mod+Y / Mod+Shift+Z', enabled: () => !!redoName() || 'Nothing to redo', run: redo },
  { id: 'edit.history', title: 'History', group: 'Edit', keywords: 'timeline undo steps time travel', run: () => void import('./timeline').then((m) => m.openTimeline()) },
  { id: 'edit.selectall', title: 'Select all', group: 'Edit', keys: 'Mod+A', enabled: objects, run: selectAll },
  { id: 'edit.deselect', title: 'Deselect all', group: 'Edit', keys: 'Esc', enabled: selection, run: () => setState({ selected: null }) },
  { id: 'edit.copy', title: 'Copy', group: 'Edit', keys: 'Mod+C', enabled: selection, run: copySelection },
  { id: 'edit.cut', title: 'Cut to clipboard', group: 'Edit', keys: 'Mod+X', enabled: selection, run: cutSelection },
  { id: 'edit.paste', title: 'Paste', group: 'Edit', keys: 'Mod+V', enabled: () => canPaste() || 'Clipboard empty', run: pasteClipboard },
  { id: 'edit.delete', title: 'Delete selected', group: 'Edit', keys: 'Del / Backspace', keywords: 'remove', enabled: selection, run: removeSelected },
  { id: 'edit.deleteall', title: 'Delete all', group: 'Edit', keys: 'Mod+Del', keywords: 'clear remove', enabled: objects, run: deleteAll },
  { id: 'edit.clone', title: 'Clone selected…', group: 'Edit', keys: 'Mod+D', keywords: 'duplicate copies', enabled: noProject(selection), run: app('clone') },
  { id: 'edit.rename', title: 'Rename', group: 'Edit', keys: 'F2', enabled: selection, run: () => startRename() },
  { id: 'edit.prefs', title: 'Preferences', group: 'Edit', keys: 'Mod+P', keywords: 'settings options', run: app('preferences') },

  { id: 'obj.arrange', title: 'Arrange all', group: 'Object', keys: 'A', keywords: 'layout pack', enabled: objects, run: () => arrangeAll(true), boost: 30 },
  { id: 'obj.arrangeplate', title: 'Arrange this plate', group: 'Object', keys: 'Shift+A', enabled: objects, run: () => arrangeAll(false) },
  { id: 'obj.orient', title: 'Auto orient', group: 'Object', keys: 'Q', keywords: 'rotate best', enabled: noProject(objects), run: () => toolbarOperation('orient'), boost: 30 },
  { id: 'obj.orientall', title: 'Auto orient all on plate', group: 'Object', keys: 'Shift+Q', enabled: noProject(objects), run: () => toolbarOperation('orient', true) },
  { id: 'obj.add', title: 'Add instance', group: 'Object', keys: '+', keywords: 'duplicate copy more', enabled: noProject(editable), run: addInstance },
  { id: 'obj.remove', title: 'Remove instance', group: 'Object', keys: '-', enabled: noProject(editable), run: removeInstance },
  { id: 'obj.count', title: 'Set number of instances…', group: 'Object', enabled: noProject(editable), run: () => void setInstanceCount() },
  { id: 'obj.fill', title: 'Fill bed with instances', group: 'Object', enabled: noProject(editable), run: fillBed },
  { id: 'obj.split', title: 'Split to objects', group: 'Object', enabled: noProject(editable), run: () => toolbarOperation('split') },
  { id: 'obj.splitparts', title: 'Split to parts', group: 'Object', enabled: noProject(editable), run: () => toolbarOperation('split-parts') },
  { id: 'obj.printable', title: 'Printable on / off', group: 'Object', keys: 'V', enabled: selection, run: () => { const sel = s().selected; if (sel) togglePrintable(sel.object, sel.allInstances ? undefined : sel.instance); } },
  { id: 'obj.center', title: 'Center on plate', group: 'Object', enabled: selection, run: centreSelection },
  { id: 'obj.drop', title: 'Drop to bed', group: 'Object', enabled: () => canDrop() || 'Already on the bed', run: dropSelection },
  ...(['X', 'Y', 'Z'] as const).map((a, i): Command => ({ id: `obj.mirror${a}`, title: `Mirror along ${a}`, group: 'Object', enabled: selection, run: () => void mirrorSelection(i as 0 | 1 | 2) })),
  { id: 'obj.table', title: 'Object table…', group: 'Object', keywords: 'list objects', enabled: objects, run: app('objectTable') },

  { id: 'tool.move', title: 'Move', group: 'Tools', keys: 'M', keywords: 'gizmo translate position', enabled: editable, run: gizmo('move') },
  { id: 'tool.rotate', title: 'Rotate', group: 'Tools', keys: 'R', keywords: 'gizmo turn', enabled: editable, run: gizmo('rotate') },
  { id: 'tool.scale', title: 'Scale', group: 'Tools', keys: 'S', keywords: 'gizmo size resize', enabled: editable, run: gizmo('scale') },
  { id: 'tool.flatten', title: 'Lay on face', group: 'Tools', keys: 'F', keywords: 'place on face flatten', enabled: editable, run: gizmo('flatten'), boost: 20 },
  { id: 'tool.cut', title: 'Cut', group: 'Tools', keys: 'C', keywords: 'gizmo split plane', enabled: () => !cutBlocked() || cutBlocked()!, run: () => { setState({ tab: 'prepare' }); toggleCut(); } },
  { id: 'tool.measure', title: 'Measure', group: 'Tools', keys: 'U', keywords: 'distance', enabled: selection, run: gizmo('measure') },
  { id: 'tool.boolean', title: 'Mesh boolean', group: 'Tools', keys: 'B', keywords: 'union difference intersection', enabled: () => canBoolean(s()) || 'Select an object with parts', run: toggleBoolean },
  { id: 'tool.text', title: 'Text', group: 'Tools', keys: 'T', keywords: 'emboss label', enabled: () => !!s().features.emboss_text || 'Not with this tool', run: toggleText },
  { id: 'tool.svg', title: 'SVG', group: 'Tools', keywords: 'emboss image logo', enabled: () => !!s().features.emboss_svg || 'Not with this tool', run: toggleSvg },
  { id: 'tool.brimears', title: 'Brim ears', group: 'Tools', keys: 'E', enabled: () => !!s().features.brim_ears ? editable() : 'Not with this tool', run: gizmo('brimears') },
  { id: 'tool.layers', title: 'Variable layer height', group: 'Tools', keywords: 'adaptive', enabled: () => canEditLayers(s()) || 'Select one object', run: gizmo('layers') },
  ...PAINTS.map(([k, title, keys]): Command => ({ id: `tool.paint.${k}`, title, group: 'Tools', keys, keywords: 'paint brush', enabled: () => canPaint(s()) || 'Select one object', run: () => { setState({ tab: 'prepare' }); togglePaint(k); } })),
  { id: 'tool.assembly', title: 'Assembly view', group: 'Tools', keywords: 'explode', enabled: objects, run: () => setAssembly({ on: !assembly().on }) },

  { id: 'slice.plate', title: 'Slice plate', group: 'Slice', enabled: () => !s().objects.length ? 'Add a model first' : busy(s()) ? 'Slicing' : outsideVolumeWarning() ?? true, run: () => void slice() },
  { id: 'slice.all', title: 'Slice all plates', group: 'Slice', enabled: () => s().projectPlates.length > 1 ? !busy(s()) || 'Slicing' : 'One plate only', run: () => void sliceAll() },
  { id: 'slice.cancel', title: 'Cancel slicing', group: 'Slice', enabled: () => busy(s()) || 'Not slicing', run: cancelSlice },

  { id: 'plate.add', title: 'Add plate', group: 'Plate', enabled: () => canAddPlate(s()) || 'Not with this tool', run: addPlate },
  { id: 'plate.duplicate', title: 'Duplicate plate', group: 'Plate', enabled: () => canAddPlate(s()) || 'Not with this tool', run: duplicatePlate },
  { id: 'plate.settings', title: 'Plate settings…', group: 'Plate', keywords: 'bed type sequence', run: app('plate') },
  { id: 'plate.flush', title: 'Flushing volumes…', group: 'Plate', keywords: 'purge wipe', run: () => setState({ flushDialog: true }) },

  { id: 'view.prepare', title: 'Model', group: 'View', keys: 'L', keywords: 'prepare tab switch 3d', run: () => setState({ tab: 'prepare' }) },
  { id: 'view.preview', title: 'Layers', group: 'View', keys: 'L', keywords: 'preview tab switch toolpaths', run: () => setState({ tab: 'preview' }) },
  { id: 'view.fit', title: 'Zoom to selection', group: 'View', keywords: 'fit frame focus', run: () => window.dispatchEvent(new Event('camera-fit')) },
  ...CAMERA_VIEWS.map((label, i): Command => ({ id: `view.cam.${VIEW_DIRECTIONS[i]}`, title: i ? `${label} view` : label, group: 'View', keys: String(i), keywords: 'camera', run: () => cameraCommand(VIEW_DIRECTIONS[i]) })),
  { id: 'view.sidebar', title: 'Show / hide sidebar', group: 'View', keys: 'Mod+\\', run: () => toggleSidebar() },
  { id: 'view.perspective', title: 'Perspective view', group: 'View', keywords: 'camera projection', run: () => cameraCommand('perspective') },
  { id: 'view.ortho', title: 'Orthogonal view', group: 'View', keywords: 'camera projection orthographic', run: () => cameraCommand('orthogonal') },
  ...VIEW_TOGGLES.map(([k, title, keys]): Command => ({ id: `view.${k}`, title, group: 'View', keys, get detail() { return viewOptions()[k] ? 'On' : 'Off'; }, run: () => toggleViewOption(k) })),
  { id: 'view.nonplanar', title: 'Show non-planar surfaces', group: 'View', keywords: 'curved slope', run: toggleNonplanarView },
  { id: 'theme.light', title: 'Theme: Light', group: 'View', keywords: 'appearance colour', run: () => setThemePref('light') },
  { id: 'theme.dark', title: 'Theme: Dark', group: 'View', keywords: 'appearance colour night', run: () => setThemePref('dark') },
  { id: 'theme.system', title: 'Theme: System', group: 'View', keywords: 'appearance colour auto', run: () => setThemePref('system') },

  { id: 'preset.printer', title: 'Printer settings…', group: 'Preset', keywords: 'machine edit', run: () => setState({ dialog: { type: 'machine', slot: 0 } }) },
  { id: 'preset.filament', title: 'Filament settings…', group: 'Preset', keywords: 'material edit', run: () => setState({ dialog: { type: 'filament', slot: 0 } }) },
  { id: 'preset.process', title: 'Process settings', group: 'Preset', keywords: 'quality edit', run: () => setState({ tab: 'prepare', processScope: 'global' }) },
  { id: 'preset.filaments', title: 'Set filaments to use…', group: 'Preset', run: app('filaments') },
  { id: 'preset.create', title: 'Create printer…', group: 'Preset', keywords: 'new machine nozzle', run: app('createPrinter') },
  { id: 'preset.search', title: 'Search settings', group: 'Preset', keys: 'Mod+F / /', keywords: 'find option', run: () => void import('./registry').then((m) => m.openPalette('/')) },

  { id: 'help.palette', title: 'Command palette', group: 'Help', keys: 'Mod+K', run: () => void import('./registry').then((m) => m.openPalette('')) },
  { id: 'help.shortcuts', title: 'Keyboard shortcuts', group: 'Help', keys: '?', run: app('shortcuts') },
  { id: 'help.develop', title: 'Develop mode', group: 'Help', keywords: 'developer expert debug', get detail() { return s().developerMode ? 'On' : 'Off'; }, run: () => { const on = !s().developerMode; savePreferences({'orca.developer_mode': on ? '1' : '0'}); setState({ developerMode: on }); } },
  { id: 'help.setup', title: 'Setup', group: 'Help', run: app('setup') },
  { id: 'help.about', title: 'About PancakeSlicer', group: 'Help', keywords: 'version licence', run: app('about') },
  { id: 'help.report', title: 'Report a problem', group: 'Help', keywords: 'bug issue feedback github', run: reportProblem },
  { id: 'help.calibguide', title: 'Calibration guide', group: 'Help', run: () => void window.open('https://www.orcaslicer.com/wiki/calibration_guide', '_blank', 'noopener') },
  ...CALIB.map(([k, title]): Command => ({ id: `calib.${k}`, title: `Calibrate: ${title}`, group: 'Calibration', enabled: () => canCalibrate() || 'Not with this tool', run: () => setState({ calibDialog: k }) })),

  { id: 'key.tab', title: 'Model / Layers (in a 3D view)', group: 'View', keys: 'Tab' },
  { id: 'key.views', title: 'Views', group: 'View', keys: '0 … 6' },
  { id: 'key.leave', title: 'Leave the 3D view', group: 'View', keys: 'Shift+Tab / Esc' },
  { id: 'key.zoom', title: 'Zoom in / out', group: 'View', keys: 'I / O' },
  { id: 'key.nudge', title: 'Move 10 mm (Shift: 1 mm)', group: 'Object', keys: 'Arrows' },
  { id: 'key.turn', title: 'Rotate 45°', group: 'Object', keys: 'PgUp / PgDn' },
  { id: 'key.orbit', title: 'Orbit', group: 'View', keys: 'Left drag' },
  { id: 'key.pan', title: 'Pan', group: 'View', keys: 'Right drag' },
  { id: 'key.wheel', title: 'Zoom', group: 'View', keys: 'Wheel' },
  { id: 'key.paint.brush', title: 'Circle / sphere', group: 'Painting', keys: 'C / S' },
  { id: 'key.paint.fill', title: 'Fill / gap fill', group: 'Painting', keys: 'F / G' },
  { id: 'key.paint.tri', title: 'Triangle / height range', group: 'Painting', keys: 'T / H' },
  { id: 'key.paint.filament', title: 'Filament', group: 'Painting', keys: '1 … 9' },
  { id: 'key.pv.layer', title: 'Layer up / down', group: 'Preview', keys: 'Up / Down' },
  { id: 'key.pv.move', title: 'Move back / forward', group: 'Preview', keys: 'Left / Right' },
  { id: 'key.pv.ends', title: 'First / last move', group: 'Preview', keys: 'Home / End' },
  { id: 'key.pv.one', title: 'One layer', group: 'Preview', keys: 'Shift+L' },
  { id: 'key.pv.goto', title: 'Go to layer', group: 'Preview', keys: 'Shift+G' },
];

export function exportTitle() {
  return outputKind(getState()) === 'gcode.3mf' ? 'Export plate sliced file' : 'Export G-code';
}

let registered = false;
export function registerCore() {
  if (registered) return;
  registered = true;
  registerCommand(CORE);
}
