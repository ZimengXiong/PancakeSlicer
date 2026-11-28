import { openUrlImport } from '../url-import';
import { addInstance, arrangeAll, fillBed, openFileDialog, removeInstance, setInstanceCount, toolbarOperation } from '../actions';
import { canDrop, canPaste, centreSelection, copySelection, dropSelection, mirrorSelection, pasteClipboard, selectAll, setFilament, startRename } from '../edit';
import { changeVolumeType, deleteAll, deleteVolume, effectiveConfig, getState, removeSelected, selectedObjectIds, setState, togglePrintable, updateObject, updateVolume, useStore, VOLUME_ORDER, type PlateObject, type State } from '../state/store';
import { addPrimitive, addPrimitiveObject, loadVolume, SEAM_MODES, SHAPES, TYPE_LABEL, VOLUME_MENU } from '../plater/volumes';
import { ActionMenu, type MenuEntry as Item } from './Menu';
import { canAddPlate, deletePlate, editPlateName } from '../plater/plates';
import { supportFinsEligibility } from '../plater/supportFins';
import { SlotChip, slotsOf } from './SlotChip';
import { count, setKeepOriginal } from '../simplify/ui';

const HANDY = [['Orca Cube'], ['OrcaSliced Combo'], ['Orca Badge'], ['Orca Tolerance Test'], ['3DBenchy'], ['Cali Cat'], ['Autodesk FDM Test'], ['Voron Cube'],
  ['Stanford Bunny'], ['Orca String Hell']] as const;

const ops = () => import('../plater/objectOps');
const exportStl = (selection: boolean, multi = false) => import('../exportStl').then((m) => m.exportStl(selection, multi));

export default function ContextMenu() {
  const menu = useStore((s) => s.contextMenu);
  const s = useStore((st) => st);
  const close = () => setState({ contextMenu: null });
  if (!menu) return null;
  const o = s.objects.find((ob) => ob.id === menu.object);
  const all = !!s.selected?.all;
  const ids = all ? selectedObjectIds(s) : [];
  const editable = !!o && !o.project?.placeholder;
  const finsReason = o && !all ? supportFinsEligibility(s, o) : null;
  const hasFins = !!o?.volumes?.some((v) => v.supportFins);
  const printable = (o?.instances.find((i) => menu.instance === undefined || i.id === menu.instance) ?? o?.instances[0])?.printable !== false;
  const volume = o?.volumes?.find((v) => v.id === menu.volume);
  const heightRanges: Item[] = o && !all && !s.project && editable ? [{ label: 'Height Range Modifier',
    action: () => void import('../plater/ranges').then((m) => { m.addHeightRanges(o.id); }) }] : [];
  const addVolumes: Item[] = o && !all && editable ? [...VOLUME_MENU.map(([type, label]) => ({ label, sub: [
    { label: 'Load...', action: () => loadVolume(o.id, type) },
    'sep' as const,
    ...SHAPES.map((shape) => ({ label: shape, action: () => addPrimitive(o.id, type, shape) })),
    ...((type === 'part' || type === 'negative' || type === 'modifier')
      ? [{ label: 'Text', action: () => void import('../plater/text').then((m) => m.addText(o.id, type)) }] : []),
    ...((type === 'part' || type === 'negative' || type === 'modifier')
      ? [{ label: 'SVG', action: () => void import('../plater/svg').then((m) => m.addSvg(o.id, type)) }] : []),
  ] })), ...heightRanges, 'sep' as const] : heightRanges.length ? [...heightRanges, 'sep'] : [];
  const items: Item[] = o && volume ? [
    { label: 'Settings', disabled: !!o.project || (volume.type !== 'part' && volume.type !== 'modifier'),
      action: () => setState({ selected: { object: o.id, instance: o.instances[0].id, allInstances: true, volume: volume.id } }) },
    ...(volume.text ? [{ label: 'Edit text', action: () => setState({ gizmo: 'text' }) }] : []),
    ...(volume.svg ? [{ label: 'Edit SVG', action: () => setState({ gizmo: 'svg' }) }] : []),
    { label: 'Rename', action: () => setState({ renaming: volume.id }) },
    { label: 'Copy Process Settings', action: () => void ops().then((m) => m.copySettings(o.id, volume.id)), tip: 'Copy Process Settings' },
    { label: 'Paste Process Settings', action: () => void ops().then((m) => m.pasteSettings([{ object: o.id, volume: volume.id }])), disabled: s.settingsClipboard?.kind !== 'volume', tip: 'Paste Process Settings' },
    { label: 'Delete', shortcut: 'Del', action: () => deleteVolume(o.id, volume.id), tip: 'Delete the selected part' },
    { label: 'Mirror', tip: 'Mirror the part', sub: ([0, 1, 2] as const).map((a) => (
      { label: `Along ${'XYZ'[a]} Axis`, action: () => void ops().then((m) => m.mirrorVolume(o.id, volume.id, a)), tip: `Mirror along the ${'XYZ'[a]} Axis` })) },
    { label: 'Export as STL...', output: true, action: () => void ops().then((m) => m.exportVolumeStl(o.id, volume.id)), tip: 'Export the part as an STL' },
    ...(volume.type === 'part' ? ['sep' as const, { label: 'Change Filament', sub: [{ label: 'Object’s filament', checked: !volume.extruder, action: () => updateVolume(o.id, volume.id, { extruder: 0 }) },
      ...slotsOf(s).map((slot) => ({ label: slot.name, icon: <SlotChip n={slot.n} colour={slot.colour} />, role: 'menuitemradio' as const, checked: volume.extruder === slot.n, action: () => updateVolume(o.id, volume.id, { extruder: slot.n }) }))] }] : []),
    'sep',
    { label: 'Change Type', tip: 'Change part type', sub: VOLUME_ORDER.map((type) => (
      { label: TYPE_LABEL[type], checked: volume.type === type, action: () => volume.type !== type && changeVolumeType(o.id, volume.id, type) })) },
    ...(volume.type === 'seam' ? [{ label: 'Precise Seam Type', sub: SEAM_MODES.map(([mode, label]) => (
      { label, checked: (volume.seamMode ?? 'center') === mode, action: () => updateVolume(o.id, volume.id, { seamMode: mode }) })) }] : []),
  ] : o ? [
    ...(all ? [] : [
      { label: 'Add instance', shortcut: '+', action: addInstance, disabled: !editable, tip: 'Add one more instance of the selected object' },
      { label: 'Remove instance', shortcut: '-', action: removeInstance, disabled: o.instances.length < 2, tip: 'Remove one instance of the selected object' },
      { label: 'Set number of instances...', action: () => void setInstanceCount(), disabled: !editable, tip: 'Change the number of instances of the selected object' },
      { label: 'Fill bed with instances...', action: fillBed, disabled: !editable, tip: 'Fill the remaining space on the bed with copies of the selected object' },
      'sep' as const,
    ]),
    ...(all && !s.project ? [{ label: 'Assemble', tip: 'Combine the selected objects into a single multi-part object', disabled: ids.length < 2,
      action: () => void import('../plater/assemble').then((m) => m.assemble(ids)) }, 'sep' as const] : []),
    { label: 'Copy', shortcut: 'Ctrl+C', action: copySelection, disabled: !editable || !!o.project },
    { label: 'Paste', shortcut: 'Ctrl+V', action: pasteClipboard, disabled: !canPaste(s) },
    { label: 'Clone', shortcut: 'Ctrl+K', action: () => setState({ appDialog: 'clone' }), disabled: !editable || !!o.project, tip: 'Make copies of the selected objects' },
    ...(!all ? [{ label: 'Fix Model', action: () => void import('../plater/meshOps').then((m) => m.fixModel(o.id)), disabled: !editable || !!o.project || !!o.volumes?.length }] : []),
    ...(!all ? [{ label: 'Simplify Model', action: () => setState({ appDialog: 'simplify' }), disabled: !editable || !!o.project || !!o.volumes?.length }] : []),
    ...(!all && o.simplification && o.simplification.trianglesAfter < o.simplification.trianglesBefore ? [o.simplification.kept === 'original'
      ? { label: 'Use Simplified Mesh', action: () => void setKeepOriginal(o.id, false), disabled: !editable || !!o.project || !!o.native,
        tip: `Go back to the ${count(o.simplification.trianglesAfter)}-triangle mesh made on import` }
      : { label: 'Use Original Mesh', action: () => void setKeepOriginal(o.id, true), disabled: !editable || !!o.project || !!o.native,
        tip: `This model was simplified from ${count(o.simplification.trianglesBefore)} to ${count(o.simplification.trianglesAfter)} triangles on import; reload all of them` }] : []),
    ...(!all ? [{ label: hasFins ? 'Regenerate Support Fins' : 'Generate Support Fins',
      action: () => void import('../plater/supportFins').then((m) => m.generateSupportFins(o.id)),
      disabled: !editable || !!finsReason || !s.api || !!s.api.mock,
      tip: finsReason ?? 'Bake the original model pose and generate separate breakaway fin geometry' }] : []),
    { label: 'Center', action: centreSelection, disabled: !editable },
    { label: 'Drop', action: dropSelection, disabled: !editable || !canDrop(s), tip: 'Drop the selection to the bed' },
    ...(all ? [] : [{ label: 'Split', disabled: !editable || !!s.project, sub: [
      { label: 'To Objects', action: () => void toolbarOperation('split'), tip: 'Split the selected object into multiple objects' },
      { label: 'To Parts', action: () => void toolbarOperation('split-parts'), disabled: !!o?.volumes?.length, tip: 'Split the selected object into multiple parts' },
    ] }]),
    ...(all ? [{ label: 'Merge', action: () => void ops().then((m) => m.mergeObjects(ids)), disabled: !!s.project || ids.length < 2,
      tip: 'Merge the selected objects into a single multi-part object' },
      { label: 'Paste Process Settings', action: () => void ops().then((m) => m.pasteSettings(ids.map((id) => ({ object: id })))), disabled: s.settingsClipboard?.kind !== 'object' || !!s.project, tip: 'Paste Process Settings' }] : [
      { label: 'Auto orient', action: () => void toolbarOperation('orient'), disabled: !editable, tip: 'Automatically orient the object for better print quality' },
      { label: 'Scale to build volume', action: () => void ops().then((m) => m.scaleToBuildVolume(o.id)), disabled: !editable || !!o.project, tip: 'Scale the selected object to fit the build volume' },
      ...(o.units ? [{ label: `Restore to ${o.units}`, action: () => void ops().then((m) => m.convertUnits(o.id, null)), disabled: !editable || !!o.project }]
        : [{ label: 'Convert from inch', action: () => void ops().then((m) => m.convertUnits(o.id, 'inch')), disabled: !editable || !!o.project },
          { label: 'Convert from meter', action: () => void ops().then((m) => m.convertUnits(o.id, 'meter')), disabled: !editable || !!o.project }]),
      ...(o.volumes?.some((v) => v.type === 'part') ? [{ label: 'Set as individual objects', action: () => void ops().then((m) => m.partsToObjects(o.id)), disabled: !editable || !!o.project,
        tip: 'Turn each part into its own separate object' }] : []),
      { label: 'Settings', action: () => setState({ selected: { object: o.id, instance: o.instances[0].id, allInstances: true } }), disabled: !!o.project },
      { label: 'Copy Process Settings', action: () => void ops().then((m) => m.copySettings(o.id)), disabled: !!o.project, tip: 'Copy Process Settings' },
      { label: 'Paste Process Settings', action: () => void ops().then((m) => m.pasteSettings([{ object: o.id }])), disabled: s.settingsClipboard?.kind !== 'object' || !!o.project, tip: 'Paste Process Settings' },
      { label: 'Replace 3D file...', action: () => void ops().then((m) => m.replaceFile(o.id)), disabled: !editable || !!o.project, tip: 'Replace the object\'s mesh with another file' },
    ]),
    { label: 'Mirror', disabled: !editable, tip: 'Mirror object', sub: ([0, 1, 2] as const).map((a) => (
      { label: `Along ${'XYZ'[a]} Axis`, action: () => void mirrorSelection(a), tip: `Mirror along the ${'XYZ'[a]} Axis` })) },
    ...(all ? [] : [{ label: 'Rename', action: () => startRename(o.id), disabled: !!o.project }]),
    { label: 'Delete', shortcut: 'Del', action: removeSelected, tip: 'Delete the selected object' },
    'sep',
    ...addVolumes,
    ...(all ? [] : [{ label: 'Change Filament', sub: slotsOf(s).map((slot) => (
      { label: slot.name, icon: <SlotChip n={slot.n} colour={slot.colour} />, role: 'menuitemradio' as const, checked: (o.extruder || 1) === slot.n, action: () => setFilament(o.id, slot.n) })) }]),
    ...(all ? [] : flushOptions(s, o)),
    { label: 'Printable', shortcut: 'V', checked: printable, action: () => togglePrintable(o.id, menu.instance) },
    { label: 'Edit in Parameter Table', action: () => setState({ appDialog: 'objectTable' }), tip: 'Edit print parameters for a single object' },
    'sep',
    { label: 'Export as one STL...', output: true, action: () => void exportStl(true), disabled: !editable },
    ...(all ? [{ label: 'Export as STLs...', output: true, action: () => void exportStl(true, true), disabled: !editable }] : []),
  ] : [
    { label: 'Add Primitive', disabled: !!s.project, sub: SHAPES.map((shape) => ({ label: shape, action: () => void addPrimitiveObject(shape) })) },
    { label: 'Add Handy models', disabled: !!s.project, sub: HANDY.map(([label], i) => ({ label, action: () => void import('../plater/handy').then((m) => m.addHandyModel(i)) })) },
    { label: 'Add SVG', disabled: !!s.project, action: () => void import('../plater/svg').then((m) => m.addSvg(null)) },
    { label: 'Add Models', shortcut: 'Ctrl+I', action: openFileDialog },
    { label: 'Import from URL…', action: () => openUrlImport() },
    { label: 'Paste', shortcut: 'Ctrl+V', action: pasteClipboard, disabled: !canPaste(s) },
    'sep',
    { label: 'Select All', shortcut: 'Ctrl+A', action: selectAll, disabled: !s.objects.length, tip: 'Select all objects on the current plate' },
    { label: 'Delete All', shortcut: 'Ctrl+Del', action: deleteAll, disabled: !s.objects.length && !getState().projectPlates.length, tip: 'Delete all objects on the current plate' },
    { label: 'Arrange', action: () => arrangeAll(false), disabled: !s.objects.length, tip: 'Arrange current plate' },
    ...(canAddPlate(s) && s.projectPlates.length > 1 ? [
      { label: 'Delete Plate', action: () => deletePlate(), tip: 'Remove the selected plate' },
      { label: 'Edit Plate Name', action: () => void editPlateName() },
    ] : []),
    ...(canAddPlate(s) ? [{ label: 'Plate Settings...', action: () => setState({ appDialog: 'plate' }), tip: 'Customize current plate' }] : []),
  ];
  return <ActionMenu items={items} label="Context menu" at={{ x: menu.x, y: menu.y }} onClose={close} />;
}

const FLUSH: [string, string][] = [['flush_into_infill', "Flush into objects' infill"], ['flush_into_objects', 'Flush into this object'], ['flush_into_support', "Flush into objects' support"]];
const on = (v: unknown) => v === true || v === 1 || v === '1' || v === 'true';

function flushOptions(s: State, o: PlateObject): Item[] {
  const used = new Set(s.objects.flatMap((x) => [x.extruder || 1, ...(x.volumes ?? []).map((v) => v.extruder || x.extruder || 1)]));
  if (used.size < 2) return [];
  const global = effectiveConfig(s, 'process');
  const tower = on(global.enable_prime_tower);
  return [{ label: 'Flush Options', sub: FLUSH.map(([key, label]) => {
    const value = on(key in o.settings ? o.settings[key] : global[key]);
    return { label, checked: value, disabled: !tower, action: () => updateObject(o.id, { settings: { ...o.settings, [key]: !value } }, label) };
  }) }];
}
