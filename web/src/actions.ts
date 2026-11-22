import { ownValues } from './profiles/view';
import { failureCode, responseError, type ApiError } from './api/access';
import { savePreferences } from './saved-preferences';
import { beginBusy, startupBusy, withBusy, type BusyToken } from './state/busy';
import { beginSlicePreview, bindSlicePreview } from './flow/preview-navigation';
import { project as projectFlow } from './flow/project';
import { app } from './flow/app';
import { importing } from './flow/import';
import { output } from './flow/output';
importing.machine.subscribe((s) => {
  if (s.value === 'running') app.send({ type: 'IMPORT' });
  else if (app.get().value !== 'booting') app.send({ type: 'SETTLED', hasObjects: getState().objects.length > 0 });
});
import type * as THREE from 'three';
import { T3 } from './three-runtime';
import type { Config, ConfigValue, JobRequest, JobStatus, PresetType } from './api/types';
import { sliceProgress } from './api/progress';
import { connect, surfaceConfigBlocker, type Api } from './api/client';
import { validateProfileSubmission } from './profiles/submission';
import { shortTime } from './preview/format';
import { calibProject, firstLayer, jobFields, outputKind, plateSignature } from './slice-input';
import { arrange, bakeStl, bedBounds, instanceBox, loadMesh, loadProject, nextFreeSpot, parsePoints, plateJson, outsideBed } from './plater/geometry';
import { projectEdits } from './plater/projectTransforms';
import { confirmDialog, numberDialog, promptDialog } from './components/Ask';
import {
  IDLE_JOB, PRESET_META, effectiveConfig, getState, markStale, newId, presetEdits, printerPreset, resolvePreset, selectPrinter, setState, toast, type JobView, type PlateObject, type State,
} from './state/store';
import { documentGeneration, leaveDocument, newDocument, undoable } from './state/history';
import { clearPlateJobs, currentPlates } from './plater/plates';
import { nativeSupportCapable } from './settings-env';
import { preferredHeight } from './profiles/context';
import { loadPreviewView } from './components/preview-loader';
import { slotFilaments } from './profiles/slots';
import { DEMO } from './demo/config';
import { recordWarnings } from './engine-warnings';

export async function init() {
  app.send({ type: 'BOOT' });
  const api = await connectService();
  setState({ api }); startupBusy.relabel('Loading presets');
  if (!api.mock) {
    void import('./plater/orientPrompt').then((m) => m.startOrientPrompt());
    void import('./plater/wipeTower').then((m) => m.startWipeTower());
  }
  if (!api.mock) void import('./printers/connections').then((m) => m.followConnections());
  await loadCatalog();
}

async function connectService(): Promise<Api> {
  for (let delay = 500, wait = 1500; ; delay = Math.min(delay * 2, 5000), wait = Math.min(wait * 2, 15_000)) {
    try { return await connect(wait); } catch {  }
    await new Promise<void>((resolve) => {
      const done = () => { clearTimeout(timer); window.removeEventListener('online', done); resolve(); };
      const timer = setTimeout(done, delay); window.addEventListener('online', done);
    });
  }
}

export async function loadCatalog() { await (await import('./profiles/catalog-startup')).loadCatalog(); }

function switchingChanges(s: State, type: PresetType, slot = 0): Config {
  const name = type === 'machine' ? s.printer : type === 'process' ? s.process : slotFilaments(s)[slot];
  const { config, record } = resolvePreset(s, type, name);
  const project = type === 'filament' ? s.projectPresets?.filaments[slot] : s.projectPresets?.[type];
  const text = (v: ConfigValue) => JSON.stringify(Array.isArray(v) ? v.map(String) : String(v));
  const baseline = Object.fromEntries(Object.entries(project ?? {}).filter(([k, v]) =>
    !PRESET_META.test(k) && (config[k] === undefined || text(v) !== text(config[k]))));
  return { ...(type === 'process' ? {} : ownValues(type, record)), ...baseline, ...presetEdits(s, type, slot) };
}

export function switchPreset(type: PresetType, slot: number, name: string, edits: 'discard' | 'transfer' = 'transfer') {
  const result=undoable(type === 'machine' ? 'Change printer' : type === 'process' ? 'Change process preset' : 'Change filament preset',
    () => applyPresetSwitch(type, slot, name, edits));
  return result;
}

function applyPresetSwitch(type: PresetType, slot: number, name: string, edits: 'discard' | 'transfer') {
  const before = getState();
  const current = type === 'machine' ? before.printer : type === 'process' ? before.process : slotFilaments(before)[slot];
  let kept = 0, dropped = 0;
  if (name === current) return { kept, dropped };
  const carry = (scope: PresetType, target: string, sourceSlot = 0) => {
    if (edits === 'discard') return {};
    const config = resolvePreset(getState(), scope, target).config;
    return Object.fromEntries(Object.entries(switchingChanges(before, scope, sourceSlot)).filter(([key]) => {
      if (PRESET_META.test(key)) return false;
      if (scope === 'filament' && (key === 'filament_colour' || key === 'default_filament_colour')) return false;
      if (key in config) { kept++; return true; }
      dropped++;
      return false;
    }));
  };
  if (type === 'machine') {
    selectPrinter(name);
    const selected = getState();
    if (selected.printer === name && selected.printer !== before.printer) savePreferences({'orca.printer':name,'orca.vendor':resolvePreset(selected,'machine',name).base?.vendor ?? ''});
    return {kept,dropped};
  }
  const carried = carry(type, name, slot);
  const preferred = type === 'process' ? preferredHeight(before, name) : undefined;
  setState((st) => {
    const filamentEdits = { ...st.filamentEdits };
    let projectPresets = st.projectPresets;
    if (type === 'filament') {
      const filaments = slotFilaments(st).map((f, i) => i === slot ? carried
        : { ...st.projectPresets?.filaments[i], ...filamentEdits[f] });
      slotFilaments(st).forEach((f) => delete filamentEdits[f]);
      delete filamentEdits[name];
      projectPresets = { machine: {}, process: {}, ...projectPresets, filaments };
    } else if (projectPresets) projectPresets = { ...projectPresets, process: {} };
    const patch: Partial<State> = {
      dirty: type === 'process' ? { ...st.dirty, process: carried } : st.dirty,
      filamentEdits,
      projectPresets,
      ...(type === 'process' ? { process: name, ...(preferred ? { processVariant: { layerHeight: preferred } } : {}) } : {}),
      job: markStale(st.job),
    };
    return patch;
  });
  if (type === 'filament') void import('./profiles/slot-writes').then(({ editStrip }) => editStrip((strip) => ({ ...strip, filaments: strip.filaments.map((f, i) => (i === slot ? name : f)) }), 'Filament'));
  void import('./profiles/printer-setups').then((m) => m.saveSelectionStrip());
  return { kept, dropped };
}

export function bedArea() {
  return parsePoints(printerPreset(getState())?.config.printable_area);
}

export async function printerBed(): Promise<THREE.Vector2[]> {
  const answer = await (await import('./flow/resolved')).selectionAnswer();
  const area = parsePoints(answer.printer.values.printable_area);
  if (area.length < 3) throw new Error(`Hmm, ${printerPreset(getState())?.name ?? 'The printer'} doesn't have a bed shape (printable_area).`);
  return area;
}

export async function importFiles(files: FileList | File[], opts: { sourceUrl?: string } = {}): Promise<number> {
  let list = [...files];
  if (!list.length) return 0;
  setState({ tab: 'prepare' });
  const generation = documentGeneration();
  let imported = 0;
  for (const file of list.filter((f) => /\.3mf$/i.test(f.name))) {
    imported += await (await import('./plater/import-project')).importObjects(file, false, opts.sourceUrl);
    if (generation !== documentGeneration()) return imported;
  }
  list = list.filter((f) => !/\.3mf$/i.test(f.name));
  if (!list.length) return imported;
  if (getState().project && false) {
    toast('youre_viewing_imported_3mf');
    return imported;
  }
  const project = !!getState().project;
  for (const f of list) void Promise.all([import('./api/blobs'), import('./persist/session')])
    .then(([{ blobDigest }, { assignBlobKey }]) => blobDigest(f).then((sum) => assignBlobKey(f, `sha256-${sum}`))).catch(() => {});
  const step = /\.(step|stp|drc)$/i;
  list = list.filter((f) => /\.(stl|obj|3mf)$/i.test(f.name) || step.test(f.name) ||
    (toast('isnt_supported_3d_model', { name: f.name, value: ', STEP' }), false));
  const task = importing.begin('loading');
  const added: PlateObject[] = [];
  for (const f of list) {
    let busy: BusyToken | undefined;
    try {
      const { geometry, source } = step.test(f.name) ? await loadStep(f, task.signal)
        : { geometry: await loadMesh(f, (pct) => { if (task.current()) { task.progress(pct, 'meshing'); const label = `Loading ${f.name} ${Math.round(pct)}%`; if (busy) busy.relabel(label); else busy = beginBusy(label, task.signal); } }), source: f };
      if (!task.current()) { geometry.dispose(); break; }
      added.push({
        id: newId(), name: source.name, geometry, simplification: geometry.userData.simplification, rotX: 0, rotY: 0, scale: [1, 1, 1], extruder: 1, source, settings: {}, ...(opts.sourceUrl ? { sourceUrl: opts.sourceUrl } : {}),
        instances: [{ id: newId(), x: 0, y: 0, rotZ: 0 }],
      });
    } catch (e) {
      if (!task.current()) break;
      task.fail(e);
      toast('couldnt_load', { name: f.name, detail: e instanceof Error ? e.message : String(e) });
    } finally {
      busy?.end();
    }
  }
  if (!task.current()) { for (const o of added) o.geometry.dispose(); return imported; }
  if (!added.length) { task.done(); return imported; }
  let area: THREE.Vector2[];
  try { area = await printerBed(); }
  catch (e) {
    for (const o of added) o.geometry.dispose();
    if (task.current()) { task.fail(e); toast('couldnt_place', { value: added.map((o) => o.name).join(', '), detail: e instanceof Error ? e.message : String(e) }); }
    return imported;
  }
  if (!task.current()) { for (const o of added) o.geometry.dispose(); return imported; }
  const simplified = added.flatMap((o) => o.simplification ? [o.simplification] : []);
  if (simplified.length) void import('./simplify/ui').then((m) => m.noteSimplified(simplified));
  undoable(added.length > 1 ? 'Add objects' : 'Add object', () => setState((s) => {
    const objects = project ? added.reduce((list, o) => {
      const spot = nextFreeSpot(list, o, area) ?? bedBounds(area).center;
      return [...list, { ...o, instances: [{ ...o.instances[0], x: spot.x, y: spot.y }] }];
    }, s.objects) : arrange([...s.objects, ...added], area);
    const last = added[added.length - 1];
    return { objects, selected: { object: last.id, instance: last.instances[0].id, allInstances: true }, job: s.job.state === 'done' ? { ...s.job, stale: true } : s.job };
  }));
  task.done();
  return imported + added.length;
}

async function loadStep(f: File, signal?: AbortSignal): Promise<{ geometry: THREE.BufferGeometry; source: File }> {
  const s = getState();
  if (!s.api || s.api.mock) throw new Error(`Oops! Importing STEP files (${f.name}) requires a connection to the slicing service.`);
  return withBusy(`Loading ${f.name}`, () => meshStep(s, f, signal), signal);
}
async function meshStep(s: ReturnType<typeof getState>, f: File, signal?: AbortSignal): Promise<{ geometry: THREE.BufferGeometry; source: File }> {
  if (!s.api) throw new Error(`Oops! Importing STEP files (${f.name}) requires a connection to the slicing service.`);
  const { library } = jobFields(s, true);
  const id = await s.api.submit({ library: { ...library, filaments: [], slots: [] }, project: {}, events: [], output: '3mf', files: [{ name: f.name, blob: f }] }, signal);
  if (signal?.aborted) { void s.api.cancel(id).catch(() => {}); throw new DOMException('Import canceled', 'AbortError'); }
  await new Promise<void>((resolve, reject) => {
    let unsubscribe = () => {};
    const abort = () => { unsubscribe(); void s.api!.cancel(id).catch(() => {}); reject(new DOMException('Import canceled', 'AbortError')); };
    signal?.addEventListener('abort', abort, { once: true });
    unsubscribe = s.api!.events(id, {
      progress() {},
      done() { unsubscribe(); signal?.removeEventListener('abort', abort); resolve(); },
      error(message) { unsubscribe(); signal?.removeEventListener('abort', abort); reject(new Error(`${f.name}: ${message}`)); },
    });
  });
  const response = await fetch(s.api.downloadUrl(id), { signal });
  if (!response.ok) throw new Error(`${f.name}: ${response.status} ${response.statusText}`);
  const stem = f.name.replace(/\.(step|stp|drc)$/i, '');
  let geometry: THREE.BufferGeometry | undefined;
  await loadProject(new File([await response.blob()], `${stem}.3mf`), { items() {}, mesh: (i, g) => { if (i === 0) geometry = g; }, missing() {} });
  if (!geometry) throw new Error(`Oops! We couldn't find any 3D geometry inside ${f.name}.`);
  const mesh = { geometry, rotX: 0, rotY: 0, scale: [1, 1, 1] } as PlateObject;
  return { geometry, source: new File([await bakeStl(mesh)], `${stem}.stl`) };
}

export async function openProject(file: File) {
  await (await import('./plater/import-project')).importObjects(file, true);
}

export function clearPlate(preserveProfiles = false) {
  clearPlateJobs();
  setState(() => ({ objects: [], selected: null, gizmo: 'none', project: null, projectPlates: [], plateIndex: 1, wipeTowerPos: {}, projectName: null, projectMaterials: null, projectSlotValues: null, projectSlots: null, events: [], ...(preserveProfiles ? {} : { projectPresets: null }), job: IDLE_JOB }));
}

let creatingProject:Promise<boolean>|null=null;
export function newProject(preserveProfiles = false):Promise<boolean> {
  if(creatingProject)return creatingProject;
  creatingProject=(async()=>{
    const offerUndo=await replacePlate('New Project','Start a new project? Anything currently on the plate will be cleared.');
    if(!offerUndo)return false;
    leaveDocument();clearPlate(preserveProfiles);newDocument(true);offerUndo();return true;
  })().finally(()=>{creatingProject=null;});
  return creatingProject;
}

async function replacePlate(title: string, question: string): Promise<(() => void) | null> {
  const s = getState();
  const projects = await import('./flow/project');
  if (projects.storing()) { try { await projects.flushProject(); } catch { return null; } }
  if (!s.objects.length && !s.projectPlates.some((p) => p.objects.length)) return () => {};
  if (!projects.storing()) return (await confirmDialog(title, question)) ? () => {} : null;
  if (title === 'New Project') return () => {};
  return () => toast('opened', undefined, { action: { label: 'Undo', run: () => { const id = projects.previousProject(); if (id) void projects.openProject(id); } } });
}

export function openProjectDialog() {
  const input = document.createElement('input');
  input.type = 'file';
  input.accept = '.3mf,.3MF';
  input.onchange = () => input.files?.[0] && void openProject(input.files[0]);
  input.click();
}

export function openFileDialog() { pickModelFiles(importFiles); }

export function pickModelFiles(onFiles: (files: FileList) => unknown) {
  const input = document.createElement('input');
  input.type = 'file';
  input.multiple = true;
  input.accept = ['stl', 'obj', '3mf', 'step', 'stp'].flatMap((e) => [`.${e}`, `.${e.toUpperCase()}`]).join(',');
  input.onchange = () => input.files && void onFiles(input.files);
  input.click();
}

export function arrangeAll(allPlates = true) {
  const s = getState();
  const locked = !!s.projectPlates.find((p) => p.index === s.plateIndex)?.locked;
  if (locked && !allPlates) return toast('plate_locked');
  if (!s.project && s.api && !s.api.mock) return locked ? toast('plate_locked') : void import('./plater/operations').then((m) => m.arrangeNative());
  const area = bedArea();
  undoable('Arrange', () => setState((st) => ({
    ...(locked ? {} : { objects: arrange(st.objects, area) }),
    ...(allPlates && !st.project ? { projectPlates: st.projectPlates.map((p) => (p.index === st.plateIndex || p.locked || !p.objects.length ? p : { ...p, objects: arrange(p.objects, area) })) } : {}),
  })));
}

export function addInstance() {
  const s = getState();
  const o = s.objects.find((ob) => ob.id === s.selected?.object);
  if (!o || o.project && (o.project.placeholder)) return;
  const spot = nextFreeSpot(s.objects, o, bedArea());
  const src = o.instances.find((i) => i.id === s.selected!.instance) ?? o.instances[0];
  const inst = { id: newId(), x: spot?.x ?? src.x, y: spot?.y ?? src.y, rotZ: src.rotZ, z: src.z, printable: src.printable };
  undoable('Increase Instances', () => setState((st) => {
    const objects = st.objects.map((ob) => (ob.id === o.id ? { ...ob, instances: [...ob.instances, inst] } : ob));
    return { objects: spot ? objects : arrange(objects, bedArea()), selected: { object: o.id, instance: inst.id } };
  }));
}

export function removeInstance() {
  const s = getState();
  const o = s.objects.find((ob) => ob.id === s.selected?.object);
  if (!o || o.instances.length < 2) return;
  const keep = o.instances.slice(0, -1);
  undoable('Decrease Instances', () => setState((st) => ({
    objects: st.objects.map((ob) => (ob.id === o.id ? { ...ob, instances: keep } : ob)),
    selected: { object: o.id, instance: keep[keep.length - 1].id },
  })));
}

export async function setInstanceCount() {
  const s = getState();
  const o = s.objects.find((ob) => ob.id === s.selected?.object);
  if (!o || o.project && (o.project.placeholder)) return;
  const num = await numberDialog('Copies of the selected object', 'Number of copies:', o.instances.length, 0, 1000);
  if (num === null || num === o.instances.length) return;
  undoable(`Set number of copies to ${num}`, () => setState((st) => {
    const cur = st.objects.find((ob) => ob.id === o.id);
    if (!cur) return {};
    if (num === 0) return { objects: st.objects.filter((ob) => ob.id !== o.id), selected: null, job: markStale(st.job) };
    let next: PlateObject = { ...cur, instances: cur.instances.slice(0, num) };
    let full = false;
    const src = cur.instances[cur.instances.length - 1];
    while (next.instances.length < num) {
      const others = [...st.objects.filter((ob) => ob.id !== o.id), next];
      const spot: { x: number; y: number } | null = full ? null : nextFreeSpot(others, next, bedArea());
      full ||= !spot;
      next = { ...next, instances: [...next.instances, { id: newId(), x: spot?.x ?? src.x, y: spot?.y ?? src.y, rotZ: src.rotZ, z: src.z, printable: src.printable }] };
    }
    const objects = st.objects.map((ob) => (ob.id === o.id ? next : ob));
    const last = next.instances[next.instances.length - 1];
    return { objects: full ? arrange(objects, bedArea()) : objects, selected: { object: o.id, instance: last.id }, job: markStale(st.job) };
  }));
}

export function fillBed() {
  const s = getState();
  const o = s.objects.find((ob) => ob.id === s.selected?.object);
  if (!o || o.project && (o.project.placeholder)) return;
  const area = bedArea();
  const src = o.instances[o.instances.length - 1];
  const withCopies = (n: number) => arrange(s.objects.map((ob) => ob.id !== o.id ? ob : { ...ob, instances: [...ob.instances,
    ...Array.from({ length: n }, () => ({ id: newId(), x: src.x, y: src.y, rotZ: src.rotZ, z: src.z, printable: src.printable }))] }), area);
  const fits = (objects: PlateObject[]) => outsideBed(objects, area, Infinity).size === 0;
  let lo = 0, hi = 1;
  let best = withCopies(0);
  if (!fits(best)) return toast('plate_full_theres_no');
  for (let next = withCopies(hi); hi <= 1000 && fits(next); next = withCopies(hi)) { best = next; lo = hi; hi *= 2; }
  while (hi - lo > 1) {
    const mid = (lo + hi) >> 1, next = withCopies(mid);
    if (fits(next)) { best = next; lo = mid; } else hi = mid;
  }
  if (!lo) return toast('plate_full_couldnt_fit');
  undoable('Arrange', () => setState((st) => ({ objects: best, selected: { object: o.id, instance: o.instances[0].id, allInstances: true }, job: markStale(st.job) })));
}

export { outputKind } from './slice-input';
export { confirmDialog, numberDialog, promptDialog };

export async function jobRequest(s = getState(), saving = false): Promise<JobRequest> {
  if (!saving) {
    if(s.profileLibraryLoad.state!=='ready')throw new Error(s.profileLibraryLoad.state==='failed'?s.profileLibraryLoad.error:'Hang tight—finishing profile setup before slicing.');
    const {flushLibraryWrites}=await import('./profiles/library-writes');await flushLibraryWrites();
    validateProfileSubmission(s);
    const [{methodAvailability},{methodSelectionProblem}]=await Promise.all([import('./profiles/method-context'),import('./profiles/capabilities')]);
    const availability=methodAvailability(s), global=effectiveConfig(s,'process');
    const effective=[global,...s.objects.flatMap(o=>[ {...global,...o.settings},...(o.volumes ?? []).map(v=>({...global,...o.settings,...v.settings})),...(o.ranges ?? []).flatMap(r=>(o.volumes?.filter(v=>v.type === 'part') ?? [undefined]).map(v=>({...global,...o.settings,...v?.settings,...r.settings})))])];
    for(const config of effective) {const problem=methodSelectionProblem(availability,config);if(problem) throw new Error(`That slicing method isn't available right now: ${problem.reason}`);}
    const [{readLibrary},{getRecord,recordKey}]=await Promise.all([import('./persist/profile-library'),import('./profiles/records')]), remote=await readLibrary();
    for(const [kind,key] of [['printer',s.printer],['process',s.process],...slotFilaments(s).filter(Boolean).map(key=>['filament',key])] as ['printer'|'process'|'filament',string][]) {
      const local=getRecord(s.records,recordKey(kind,key)), persisted=getRecord(remote,recordKey(kind,key));
      if(!local || !persisted || local.revision!==persisted.revision) throw new Error('Heads up: one of your selected profiles was updated elsewhere. Reload it or save your current edits before slicing.');
    }
    const configs = [effectiveConfig(s, 'process'), ...s.objects.flatMap((o) =>
      [o.settings, ...o.volumes?.map((v) => v.settings) ?? [], ...o.ranges?.map((r) => r.settings) ?? []])];
    for (const config of configs) {
      const reason = surfaceConfigBlocker(config);
      if (reason) throw new Error(reason);
    }
  }
  const fields = jobFields(s, saving);

  if (s.project) {
    const { added, volumes } = fields.project_edits ? projectEdits(s) : { added: [], volumes: [] };
    const { files } = added.length ? await plateJson(added, firstLayer(s)) : { files: [] };
    const parts = await Promise.all(volumes.map(({ o, k }) => bakeStl({ ...o, geometry: o.volumes![k].geometry }, new T3.Matrix4())));
    return { ...fields, files: [{ name: s.project.name, blob: s.project as Blob }, ...files, ...volumes.map(({ name }, i) => ({ name, blob: parts[i] }))] };
  }
  if (saving && (s.projectPlates.length > 1 || s.projectPlates.some((p) => Object.keys(p.settings).length || p.name || p.locked))) {
    const plates = currentPlates(s);
    const { plate, files } = await plateJson(plates.flatMap((p) => p.objects), firstLayer(s));
    plates.flatMap((p) => p.objects.map(() => p.index)).forEach((index, k) => { plate.objects[k].plate = index; });
    plate.plates = plates.map((p) => ({ index: p.index, name: p.name, settings: Object.fromEntries(Object.entries(p.settings).map(([k, v]) => [k, String(v)])), ...(p.locked ? { locked: true } : {}) }));
    return { ...fields, plate, files };
  }
  const { plate, files } = await plateJson(s.objects, firstLayer(s));
  return { ...fields, plate, files };
}

(window as unknown as { __jobId: unknown }).__jobId = () => getState().job.id;
(window as unknown as { __signature: unknown }).__signature = () => plateSignature(getState());
(window as unknown as { __job: unknown }).__job = async () => {
  const r = await jobRequest();
  return {
    files: r.files.map((f) => `${f.name} (${f.blob.size} bytes)`),
    library: JSON.stringify({ ...r.library, project: r.project }), plate: r.plate && JSON.stringify(r.plate), project_instances: r.project_instances,
    events: r.events.length ? JSON.stringify(r.events) : undefined, output: r.output,
  };
};

let sliceGeneration = 0;

export function outsideVolumeWarning(s = getState()): string | null {
  const machine = printerPreset(s);
  if (!machine || machine.config.printable_area === undefined) return null;
  const objects = s.objects.filter((o) => !o.project?.placeholder);
  if (!objects.length) return null;
  const area = parsePoints(machine.config.printable_area), height = Number(machine.config.printable_height ?? 1e6);
  const outside = outsideBed(objects, area, height), bed = bedBounds(area), xy = (v: { x: number; y: number }) => `[${v.x.toFixed(3)}, ${v.y.toFixed(3)}]`;
  const lines = objects.flatMap((o) => o.instances.filter((i) => outside.has(i.id)).slice(0, 1).map((i) => {
    const b = instanceBox(o, i);
    return b.max.z > height + 1e-3 ? `${o.name} is ${b.max.z.toFixed(1)} mm tall; the printer prints up to ${height} mm.`
      : `${o.name} sticks out past the edge of the plate (Model XY ${xy(b.min)}–${xy(b.max)}, printable area ${xy(bed.min)}–${xy(bed.max)}).`;
  }));
  return lines.length ? `${lines.join('\n')}\nMove ${lines.length > 1 ? 'them' : 'it'} fully onto the print bed, then slice again.` : null;
}

export interface JobSink {
  progress(p: { pct: number; stage: string; id?: string }): void;
  done(job: JobView): void;
  failed(error: string, code: string): void;
}

export function runJob(api: Api, request: JobRequest, signature: string, sink: JobSink, calib?: string): () => void {
  let live = true, id: string | undefined, stop: (() => void) | undefined;
  const submission = new AbortController();
  void (async () => {
    try {
      id = await api.submit(request, submission.signal, (fraction) => live && sink.progress({ pct: Math.round(fraction * 100), stage: 'uploading' }));
      if (!live) return void api.cancel(id).catch(() => undefined);
      sink.progress({ id, pct: 0, stage: 'queued' });
      const calibLog: string[] = [];
      let pct = 0, resulted = false;
      const complete = (status: JobStatus) => sink.done({ id: id!, state: 'done', pct: 100, stage: 'done', status, plateSignature: signature, warnings: recordWarnings(status.record), ...(calib ? { calib, calibLog } : {}) });
      stop = api.events(id, {
        progress: (e) => {
          if (!live || resulted) return;
          const progress = sliceProgress(e);
          if (e.stage === 'verify' && e.pct >= 100 && request.output === 'gcode.3mf') progress.stage = 'Packaging sliced plate';
          if (progress.pct >= pct || progress.queuePosition) sink.progress({ id, ...progress });
          pct = Math.max(pct, progress.pct);
        },
        result: (status) => {
          if (!live || resulted) return;
          resulted = true;
          complete(status);
        },
        done: (status) => {
          if (!live) return;
          live = false;
          complete(status);
        },
        error: (msg, code) => {
          if (!live) return;
          live = false;
          sink.failed(msg, code);
        },
        log: (stream, line) => {
          if (stream === 'stdout' && line.startsWith('calib_')) calibLog.push(line);
        },
      });
    } catch (e) {
      if (!live) return;
      live = false;
      sink.failed(String(e), failureCode(e));
    }
  })();
  return () => {
    const wasLive = live;
    live = false;
    submission.abort();
    stop?.();
    if (wasLive && id) void api.cancel(id).catch(() => undefined);
  };
}

export async function plateJob(s = getState(), ask = true): Promise<JobRequest | string | null> {
  const offers = await import('./flow/project-slots');
  if (offers.offerUndecided()) return offers.OFFER_UNDECIDED;
  const outside = outsideVolumeWarning(s);
  if (outside) return outside;
  const slots = await import('./printers/slot-check'), issues = slots.slotIssues(s);
  if (issues.missing.size) {
    const text = [`${slots.missingText(issues)}.`, ...(issues.empty.size ? [`${slots.emptyText(issues)}. Choose its filament under the printer.`] : [])].join('\n');
    if (!ask || !(await confirmDialog('Oops, that slot isn\'t available', text, ['Move to a free slot', 'Cancel']))) return ask ? null : text;
    slots.remapMissing(issues);
    return plateJob(getState(), ask);
  }
  if (issues.empty.size) return `${slots.emptyText(issues)}. Choose its filament under the printer first.`;
  if (nativeSupportCapable(s) && (String(effectiveConfig(s, 'process').support_style) === 'native' || s.objects.some((o) => o.settings.support_style === 'native'))) {
    const conflicts = (await import('./plater/nativeSupport')).nativeSupportConflicts(s);
    if (conflicts && !ask) return conflicts;
    if (conflicts && !(await confirmDialog('Native support', `${conflicts} Would you like to slice anyway?`))) return null;
  }
  return jobRequest(slots.slicedSlots(s));
}

let cancelJob: (() => void) | null = null;

export async function slice(calib?: { kind: string; params: string; head?: string }) {
  if (DEMO) return void toast('demo_no_engine');
  const [{methodAvailability}, {methodSelectionProblem}] = await Promise.all([import('./profiles/method-context'), import('./profiles/capabilities')]);
  const s = getState();
  if (!s.api || !s.objects.length && !calib || s.job.state === 'running' || s.job.state === 'submitting') return;
  const methodProblem = methodSelectionProblem(methodAvailability(s), effectiveConfig(s,'process'));
  if(methodProblem) return void setState({job:{state:'failed',pct:0,stage:'',error:`That slicing method isn't available right now: ${methodProblem.reason}`,errorCode:'slice_refused'}});
  const generation = ++sliceGeneration, document = documentGeneration(), projectId = projectFlow.get().context.id, plate = s.plateIndex;
  const current = () => generation === sliceGeneration && document === documentGeneration() && projectId === projectFlow.get().context.id && plate === getState().plateIndex;
  const signature = plateSignature(s);
  const { library, output } = jobFields(s);
  const request = calib ? { library, project: calibProject(s, calib.kind), output, events: [], files: [], calib: calib.kind, calib_params: calib.params, calib_head: calib.head }
    : await plateJob(s).catch((e: unknown) => String(e));
  if (!current() || request === null) return;
  if (typeof request === 'string') return void setState({ job: { state: 'failed', pct: 0, stage: '', error: request, errorCode: 'slice_refused' } });
  const navigation = beginSlicePreview(signature);
  void loadPreviewView();
  setState({ tab: 'prepare', job: { state: 'submitting', pct: 0, stage: 'uploading', plateSignature: signature, calib: calib?.kind } });
  cancelJob = runJob(s.api, request, signature, {
    progress: ({ id, ...p }) => { if (!current()) return; bindSlicePreview(navigation, id); setState(st => id ? {job: {...st.job, id, state: 'running', ...p}} : st.job.state === 'submitting' ? {job: {...st.job, ...p}} : {}); },
    done: (job) => { if (!current()) return; bindSlicePreview(navigation, job.id); setState({job: {...job, stale: plateSignature(getState()) !== signature}}); },
    failed: (error, errorCode) => { if (current()) setState(st => ({job: {...st.job, state: 'failed', error, errorCode}})); },
  }, calib?.kind);
}

export function cancelSlice() {
  void import('./flow/slice').then((m) => m.cancelSlice());
  ++sliceGeneration;
  cancelJob?.();
  cancelJob = null;
  setState({ job: { state: 'canceled', pct: 0, stage: '' } });
}

export function exportFilename(s = getState()): string {
  const base = s.job.calib ?? (s.project?.name ?? s.objects[0]?.name)?.replace(/\.(stl|obj|3mf|drc)$/i, '') ?? 'plate';
  const time = Number((s.job.status?.record as { text_summary?: Record<string, string> } | undefined)?.text_summary?.estimated_time_s);
  const firstTool = Math.max(0, Math.min(...s.objects.map((o) => o.extruder - 1)));
  const vars: Config = {
    ...effectiveConfig(s, 'machine'), ...effectiveConfig(s, 'filament'), ...effectiveConfig(s, 'process'),
    input_filename_base: base, initial_tool: firstTool, initial_extruder: firstTool,
    ...(Number.isFinite(time) ? { print_time: shortTime(time) } : {}),
  };
  let unknown = false;
  const at = (v: ConfigValue | undefined, i: number) => {
    const list = Array.isArray(v) ? v : typeof v === 'string' && v.includes(';') ? v.split(';') : [v];
    return list[Math.min(i, list.length - 1)];
  };
  const name = String(vars.filename_format ?? '').replace(/[{[]([a-z_]+)(?:\[([a-z_0-9]+)\])?[}\]]/g, (_, key: string, idx?: string) => {
    const v = idx === undefined ? vars[key] : at(vars[key], /^\d+$/.test(idx) ? Number(idx) : Number(vars[idx] ?? 0));
    if (v === undefined || v === '') unknown = true;
    return String(Array.isArray(v) ? v[0] : v);
  });
  const stem = !name || unknown ? base : name.replace(/\.gcode$/i, '').replace(/[<>:/\\|?*"]/g, '_');
  return `${stem}.${outputKind(s)}`;
}

export function canExport(s = getState()): boolean {
  return s.job.state === 'done' && !!s.job.id && !!s.job.plateSignature && !s.job.stale &&
    s.job.plateSignature === plateSignature(s) && !outsideVolumeWarning(s) && !curveRefused(s);
}

export const curveRefused = (s: State) => !!(s.job.status?.record as { could_not_curve?: unknown[] } | undefined)?.could_not_curve?.length;

export function fileUse(machine = printerPreset(getState())) {
  const printer = machine?.model?.replace(/^Bambu Lab /, '') ?? 'printer';
  return machine?.vendor === 'BBL'
    ? `Copy the file onto your ${printer}'s microSD card to start printing.`
    : `Copy the file onto your ${printer}'s USB drive to start printing.`;
}

export const STALE_RESULT_TIP = 'Settings changed since the last slice; slice again';

function exportBlocked(s: State) {
  if (!s.objects.length) return toast('export_nothing_to_export');
  const outside = outsideVolumeWarning(s);
  if (outside) return toast('export_outside_volume', { detail: outside });
  if (s.job.state === 'running' || s.job.state === 'submitting') return toast('export_still_slicing');
  if (s.job.state === 'done' && !s.job.stale && curveRefused(s)) return toast('export_could_not_curve');
  return s.job.state === 'done' ? toast('export_stale_slice') : toast('export_not_sliced');
}

export async function download() {
  if (DEMO) return void toast('demo_no_engine');
  const s = getState();
  if (!s.api || !canExport(s) || !s.job.id) return void (s.api && exportBlocked(s));
  const task = output.begin('exporting');
  let href: string;
  try {
    const r = await fetch(s.api.downloadUrl(s.job.id), { signal: task.signal });
    if (!r.ok) throw r.status === 409 ? await responseError(r) : new Error(`${r.status} ${r.statusText}`);
    href = URL.createObjectURL(await r.blob());
  } catch (e) {
    if (!task.current()) return;
    task.fail(e);
    if ((e as ApiError).code === 'could_not_curve') toast('export_refused_curve', { detail: (e as ApiError).message });
    else toast('export_failed', { detail: e instanceof Error ? e.message : e });
    return;
  }
  if (!task.current() || getState().job.id !== s.job.id || !canExport()) {
    URL.revokeObjectURL(href);
    return;
  }
  const a = document.createElement('a');
  a.href = href;
  a.download = exportFilename();
  setState((st) => ({ job: { ...st.job, exported: a.download } }));
  document.body.appendChild(a);
  a.click();
  task.done();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(href), 60000);
}

export const toolbarOperation = (operation: 'orient' | 'split' | 'split-parts', all = false) =>
  import('./plater/operations').then((m) => m.run(operation, all));

export async function saveProject(as = false) {
  if (import.meta.env.MODE === 'demo') return void toast('not_available_demo_mode');
  const saving = await import('./saveProject');
  return saving.saveProject(as);
}

export function toggleText() {
  const s = getState();
  if (s.tab !== 'prepare') return;
  if (s.gizmo === 'text') return setState({ gizmo: 'none' });
  void import('./plater/text').then((m) => {
    if (m.editedText()) return setState({ gizmo: 'text' });
    const o = s.objects.find((x) => x.id === s.selected?.object);
    if (o && (o.project?.placeholder || o.native || s.selected?.all)) return toast('select_one_object_plate');
    void m.addText(o ? o.id : null);
  });
}

export const canBoolean = (s: State) => {
  const o = s.objects.find((x) => x.id === s.selected?.object);
  return !!o && !s.selected?.all && !o.project?.placeholder && !o.native && !!o.volumes?.length;
};
export function toggleBoolean() {
  const s = getState();
  if (s.gizmo === 'boolean' || canBoolean(s)) setState({ gizmo: s.gizmo === 'boolean' ? 'none' : 'boolean' });
}

export function toggleSvg() {
  const s = getState();
  if (s.tab !== 'prepare') return;
  if (s.gizmo === 'svg') return setState({ gizmo: 'none' });
  void import('./plater/svg').then((m) => {
    if (m.editedSvg()) return setState({ gizmo: 'svg' });
    const o = s.objects.find((x) => x.id === s.selected?.object);
    if (o && (o.project?.placeholder || o.native || s.selected?.all)) return toast('select_one_object_plate_2');
    void m.addSvg(o ? o.id : null);
  });
}
