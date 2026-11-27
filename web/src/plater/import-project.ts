import { noticeText } from '../notice-codes';
import { currentPlate } from '../settings/common';
import { plateLabel, plateSetup } from '../profiles/plate-setup';
import type * as THREE from 'three';
import type { PlateObject, State } from '../state/store';
import { T3 } from '../three-runtime';
import type { ProjectItem } from './threemf';
import { archivePainting } from './paint';
import { getState, markStale, newId, selectPrinter, setState, toast } from '../state/store';
import { documentGeneration, undoable } from '../state/history';
import { beginBusy } from '../state/busy';
import { importing } from '../flow/import';
import { importObjectsDialog, importObjectsPrompt, type ImportObjectsChoice, type ImportObjectsPrompt } from '../components/Ask';
import { newProject, printerBed } from '../actions';
import { bakeStl, bedBounds, centred, instanceBox, loadProject, nextFreeSpot, outsideBed } from './geometry';
import { listedPlates } from './plates';
import { slotFilaments } from '../profiles/slots';
import { OPTIONS } from '../options';

type ImportedPlate = { index: number; objects: PlateObject[] };

function placeObjects(existing: PlateObject[], incoming: PlateObject[], area: THREE.Vector2[]): PlateObject[] {
  const placed = [...existing], taken = [...existing];
  for (const object of incoming) {
    let next = object;
    const box = instanceBox(object, object.instances[0]);
    const overlaps = taken.some((o) => o.instances.some((i) => {
      const other = instanceBox(o, i);
      return box.min.x < other.max.x - 0.001 && box.max.x > other.min.x + 0.001 &&
        box.min.y < other.max.y - 0.001 && box.max.y > other.min.y + 0.001 &&
        box.min.z < other.max.z - 0.001 && box.max.z > other.min.z + 0.001;
    }));
    if (overlaps || outsideBed([object], area, Infinity).size) {
      const spot = nextFreeSpot(placed, object, area) ?? bedBounds(area).center;
      next = { ...object, instances: object.instances.map((i) => ({ ...i, x: spot.x, y: spot.y })) };
      taken.push(next);
    }
    placed.push(next);
  }
  return placed;
}

function objectsPatch(state: State, imported: ImportedPlate[], area: THREE.Vector2[]): Partial<State> {
  const slots = Math.max(1, slotFilaments(state).length);
  const plates = listedPlates(state).map((p) => ({ ...p }));
  let nextIndex = Math.max(...plates.map((p) => p.index)) + 1;
  for (const [i, incoming] of imported.entries()) {
    const index = i === 0 ? state.plateIndex : nextIndex++;
    const previous = plates.find((p) => p.index === index);
    // Keep distinct slots on imported assemblies so they can be remapped together.
    const objects = incoming.objects.map((o) => o.volumes?.length ? o : { ...o, extruder: Math.max(1, Math.min(slots, o.extruder)) });
    if (previous) previous.objects = placeObjects(previous.objects, objects, area);
    else plates.push({ index, name: '', origin: [0, 0], settings: {}, events: [], objects: placeObjects([], objects, area) });
  }
  const active = plates.find((p) => p.index === state.plateIndex)!;
  const last = active.objects.at(-1);
  return { objects: active.objects, projectPlates: plates.length > 1 || state.projectPlates.length ? plates : [],
    selected: last ? { object: last.id, instance: last.instances[0].id, allInstances: true } : null,
    tab: 'prepare', job: markStale(state.job) };
}

type ImportObjectsOptions = {
  allowNew?: boolean;
  newProject?: boolean;
  sourceUrl?: string;
  canImport?: () => boolean;
  choose?: (prompt: ImportObjectsPrompt) => ImportObjectsChoice | null | Promise<ImportObjectsChoice | null>;
};

export async function importObjects(file: File, options: ImportObjectsOptions = {}): Promise<number> {
  const { allowNew = false, newProject: replaceProject = false, sourceUrl, canImport = () => true, choose = importObjectsDialog } = options;
  if (!canImport()) return 0;
  const generation = documentGeneration();
  let task = importing.begin('loading');
  const busy = beginBusy(`Reading ${file.name}`, task.signal);
  const plates = new Map<number, ImportedPlate>();
  let items: ProjectItem[] = [], skipped: string[] = [];
  const objects: PlateObject[] = [];
  const parents = new Map<number, PlateObject>();
  const byItem = new Map<string, PlateObject[]>();
  let committed = false;
  try {
    await loadProject(file, {
      items(list, materials) { items = list; skipped = materials.skipped ?? []; },
      mesh(i, geometry, centre) {
        if (!task.current()) { geometry.dispose(); return; }
        const item = items[i];
        const paint = item.paint ? archivePainting(item.paint) ?? undefined : undefined;
        if (item.part) {
          const parent = parents.get(item.part.parent)!;
          const origin = parent.instances[0];
          const [mesh, back] = centred(geometry);
          geometry.dispose();
          const matrix = new T3.Matrix4().makeTranslation(centre.x - origin.x, centre.y - origin.y, centre.z - (origin.z ?? 0)).multiply(back);
          (parent.volumes ??= []).push({ id: newId(), name: item.name, type: item.part.type, seamMode: item.part.seamMode,
            geometry: mesh, matrix: matrix.elements, extruder: item.extruder, settings: {}, paint });
          return;
        }
        const triangles = (geometry.index?.count ?? geometry.attributes.position.count) / 3;
        const object: PlateObject = { id: newId(), name: item.name, geometry, rotX: 0, rotY: 0, scale: [1, 1, 1], ...(paint && Object.keys(paint).length ? { paint } : {}),
          extruder: item.extruder, settings: {}, source: new File([], 'object.stl'),
          simplification: { trianglesBefore: triangles, trianglesAfter: triangles, maxErrorMm: 0, kept: 'original' }, ...(sourceUrl ? { sourceUrl } : {}), instances: [{ id: newId(), x: centre.x, y: centre.y, z: centre.z, rotZ: 0 }] };
        parents.set(i, object);
        objects.push(object); byItem.set(item.id, [...byItem.get(item.id) ?? [], object]);
        const plate = plates.get(item.plateIndex) ?? { index: item.plateIndex, objects: [] };
        plate.objects.push(object); plates.set(item.plateIndex, plate);
      },
      missing() { skipped.push('objects without meshes'); },
    }, true);
    busy.end();
    if (!task.current() || !canImport()) return 0;
    if (!objects.length) throw new Error(`${file.name}: no 3D meshes found in this file`);
    for (const o of objects) {
      const [instance] = o.instances;
      const bottom = instanceBox(o, instance).min.z;
      instance.z = (instance.z ?? 0) - Math.min(0, bottom);
    }
    const hasSettings = skipped.includes('settings');
    const prompt = importObjectsPrompt(file.name, objects.length, [...new Set(skipped.filter((s) => s !== 'settings'))], allowNew, hasSettings);
    const choice = await choose(prompt);
    if (!choice || !task.current() || generation !== documentGeneration() || !canImport()) return 0;
    let project: import('../profiles/library-io').ProjectSettings | null = null, settingsNote: string | null = null;
    if (choice === 'settings') try { project = await (await import('../profiles/library-io')).decodeProjectSettings(file); }
    catch (e) { const why = e instanceof Error ? e.message : String(e); settingsNote = /no project settings/i.test(why) ? 'No project settings in this file; its objects were imported.' : `Its settings couldn't be read (${why}); its objects were imported.`; }
    if (!task.current() || generation !== documentGeneration() || !canImport()) return 0;
    const [{ blobDigest }, { assignBlobKey }] = await Promise.all([import('../api/blobs'), import('../persist/session')]);
    await Promise.all(objects.map(async (o) => {
      o.source = new File([await bakeStl(o)], `${o.name.replace(/\.[^.]+$/, '') || 'object'}.stl`, { type: 'model/stl' });
      assignBlobKey(o.source, `sha256-${await blobDigest(o.source)}`);
    }));
    if (!task.current() || !canImport()) return 0;
    if (replaceProject || choice === 'new') {
      task.done();
      if (!await newProject(true)) return 0;
      if (!canImport()) return 0;
      task = importing.begin('loading');
    }
    const placing = documentGeneration();
    const report = project ? applyProjectProfiles(project, byItem) : [];
    const area = await printerBed();
    if (!task.current() || placing !== documentGeneration() || !canImport()) return 0;
    undoable('Import objects', () => setState((state) => objectsPatch(state, [...plates.values()].sort((a, b) => a.index - b.index), area)));
    committed = true;
    if (project) reportProjectSettings(project, report);
    else if (settingsNote) toast('import_project_failed', { name: file.name, settingsNote });
    task.done();
    return objects.length;
  } catch (error) {
    if (task.current()) { task.fail(error); toast('couldnt_load_3', { name: file.name, detail: await readableLoadError(file, error) }); }
    return 0;
  } finally {
    busy.end();
    if (!committed) for (const o of objects) {
      o.geometry.dispose();
      o.volumes?.forEach((v) => v.geometry.dispose());
    }
    if (task.current()) task.done();
  }
}

async function readableLoadError(file: Blob, error: unknown): Promise<string> {
  const head = new Uint8Array(await file.slice(0, 4).arrayBuffer().catch(() => new ArrayBuffer(0)));
  const zip = head.length === 4 && head[0] === 0x50 && head[1] === 0x4b && head[2] === 0x03 && head[3] === 0x04;
  return zip ? (error instanceof Error ? error.message : String(error)) : 'it isn\'t a 3MF project (not a valid ZIP archive).';
}

function applyProjectProfiles(project: import('../profiles/library-io').ProjectSettings, byItem: Map<string, PlateObject[]>): string[] {
  const report: string[] = [];
  if (project.settings && project.printer.library && project.printer.library !== getState().printer) {
    try { selectPrinter(project.printer.library); } catch (e) { report.push(e instanceof Error ? e.message : String(e)); }
  } else if (project.settings && !project.printer.library) report.push(`Your library has no ${project.printer.variant ?? project.printer.name}; add it to use this project's printer settings.`);
  if (project.settings && !project.print.library) report.push(`Your library has no ${project.print.product ?? project.print.name} print profile; its values were applied to ${getState().process ? 'your current one' : 'no print profile'}.`);
  undoable('Import project settings', () => setState((s) => ({ process: project.print.library || s.process,
    dirty: { machine: { ...s.dirty.machine, ...project.machine }, process: { ...s.dirty.process, ...project.process } },
    projectSlotValues: project.slots.map((v) => Object.keys(v).length ? v : null), job: markStale(s.job) })));
  for (const [item, list] of byItem) {
    const values = project.objects.find((p) => p.id === item)?.values;
    if (values) for (const o of list) o.settings = { ...o.settings, ...values };
  }
  return report;
}

function reportProjectSettings(project: import('../profiles/library-io').ProjectSettings, report: string[]) {
  const mounted = currentPlate(getState());
  if (project.bedType && mounted && project.bedType !== mounted)
    report.push(`This project was set up for the ${plateLabel(plateSetup(getState()), project.bedType)}; it prints on the printer's ${plateLabel(plateSetup(getState()), mounted)}.`);
  void import('../flow/project-slots').then(({ offerStrip }) => offerStrip({ ids: project.filaments.map(() => undefined), filaments: project.filaments.map((f) => f?.library || `unknown:${f?.product ?? f?.name ?? ''}`), colours: [], mixes: {} },
    project.slots));
  if (project.notStored.length) report.push(notImported(project.notStored));
  const what = project.settings ? noticeText('imported_project_settings', undefined) : noticeText('imported_object_settings', { count: project.objects.length });
  if (report.length) toast('project_imported_with_notes', { what, notes: report.join('\n') });
  else toast('project_imported', { what });
}

function notImported(entries: string[]): string {
  const shown: string[] = [];
  let internal = 0;
  for (const entry of entries) {
    const m = /^([a-z][a-z0-9_]*)(?=[ (]|$)([^(]*)/.exec(entry);
    if (!m || !m[1].includes('_') && !OPTIONS[m[1]]) { shown.push(entry); continue; }
    const label = OPTIONS[m[1]]?.label;
    if (label) shown.push(`${label}${m[2].trimEnd()}`); else internal++;
  }
  const list = [...new Set(shown)], more = list.length > 5 ? `, and ${list.length - 5} more` : '';
  const rest = internal ? `${list.length ? ', and ' : ''}${internal} internal setting${internal === 1 ? '' : 's'}` : '';
  return `Skipped importing: ${list.slice(0, 5).join('; ')}${more}${rest}.`;
}
