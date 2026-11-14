import { responseError, type ApiError } from './api/access';
import { savePreferences } from './saved-preferences';
import { beginBusy, startupBusy, withBusy, type BusyToken } from './state/busy';
import { findProfile } from './profiles/identity';
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
import { canEditProject, canLoadStep, connect, surfaceConfigBlocker, toolFeatures, type Api } from './api/client';
import { validateProfileSubmission } from './profiles/submission';
import { normalizeUserPreset } from './profiles/model';
import { shortTime } from './preview/format';
import { flushReady } from './flush-state';
import { calibOverrides, firstLayer, jobFields, outputKind, overrides, plateSignature } from './slice-input';
import { arrange, bakeStl, bedBounds, loadMesh, loadProject, nextFreeSpot, parsePoints, plateJson, outsideBed } from './plater/geometry';
import { projectEdits } from './plater/projectTransforms';
import { confirmDialog, numberDialog, promptDialog } from './components/Ask';
import {
  PRESET_META, effectiveConfig, getState, markStale, newId, presetEdits, printerPreset, resolvePreset, selectPrinter, setState, toast, type JobView, type PlateObject, type State,
} from './state/store';
import { captureUndo, documentGeneration, historyVersion, leaveDocument, newDocument, undoable } from './state/history';
import { clearPlateJobs, currentPlates } from './plater/plates';
import { nativeSupportCapable } from './settings-env';
import { capturePrinterMaterials, selectionNotice } from './profiles/material-library';
import { preferredHeight } from './profiles/context';
import { loadPreviewView } from './components/preview-loader';
import { SEP } from './separator';

export async function init() {
  app.send({ type: 'BOOT' });
  const api = await connectService();
  setState({ api }); startupBusy.relabel('Loading presets');
  if (!api.mock) {
    setState({ features: toolFeatures });
    if (toolFeatures.orient_scores === true) void import('./plater/orientPrompt').then((m) => m.startOrientPrompt());
    if (toolFeatures.wipe_tower === true) void import('./plater/wipeTower').then((m) => m.startWipeTower());
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
  const name = type === 'machine' ? s.printer : type === 'process' ? s.process : s.filamentSlots[slot];
  const { config, user } = resolvePreset(s, type, name);
  const project = type === 'filament' ? s.projectPresets?.filaments[slot] : s.projectPresets?.[type];
  const text = (v: ConfigValue) => JSON.stringify(Array.isArray(v) ? v.map(String) : String(v));
  const baseline = Object.fromEntries(Object.entries(project ?? {}).filter(([k, v]) =>
    !PRESET_META.test(k) && (config[k] === undefined || text(v) !== text(config[k]))));
  return { ...(type === 'process' ? {} : user?.overrides), ...baseline, ...presetEdits(s, type, slot) };
}

export function switchPreset(type: PresetType, slot: number, name: string, edits: 'discard' | 'transfer' = 'transfer') {
  const before=historyVersion();
  const result=undoable(type === 'machine' ? 'Change printer' : type === 'process' ? 'Change process preset' : 'Change filament preset',
    () => applyPresetSwitch(type, slot, name, edits));
  const summary=getState().lastProfileTransition;
  const changed=summary?.changes.filter(c=>c.from) ?? [];
  if(type==='machine' && historyVersion()!==before && summary && (changed.length || summary.needsSelection)){
    const undoTransition=captureUndo();
    const replaced=selectionNotice(getState(),summary);
    toast(replaced || changed.map(c=>`${c.kind==='process'?'Print settings':`Slot ${(c.slot??0)+1}`}: ${c.to ?? c.reason}`).join(SEP),{label:'Undo',run:()=>{if(!undoTransition())toast('Can\'t undo the printer switch because you\'ve made newer changes since then (your latest work is safe!).');}},undefined,replaced?'warning':undefined);
  }
  return result;
}

function applyPresetSwitch(type: PresetType, slot: number, name: string, edits: 'discard' | 'transfer') {
  const before = getState();
  const current = type === 'machine' ? before.printer : type === 'process' ? before.process : before.filamentSlots[slot];
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
      const filaments = st.filamentSlots.map((f, i) => i === slot ? carried
        : { ...st.projectPresets?.filaments[i], ...filamentEdits[f] });
      st.filamentSlots.forEach((f) => delete filamentEdits[f]);
      delete filamentEdits[name];
      projectPresets = { machine: {}, process: {}, ...projectPresets, filaments };
    } else if (projectPresets) projectPresets = { ...projectPresets, process: {} };
    const patch: Partial<State> = {
      dirty: type === 'process' ? { ...st.dirty, process: carried } : st.dirty,
      filamentEdits,
      projectPresets,
      ...(type === 'process' ? { process: name, ...(preferred ? { processVariant: { layerHeight: preferred } } : {}) } : { filamentSlots: st.filamentSlots.map((f, i) => (i === slot ? name : f)),
        slotColours: st.mixes[slot] ? st.slotColours : st.slotColours.map((c, i) => (i === slot ? undefined : c)) }),
      job: markStale(st.job),
    };
    return st.profileLibraryLoad.state === 'ready' ? { ...patch, printerLibrary: capturePrinterMaterials({ ...st, ...patch }) } : patch;
  });
  return { kept, dropped };
}

export function bedArea() {
  return parsePoints(printerPreset(getState())?.config.printable_area);
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
  if (getState().project && !canEditProject()) {
    toast('You\'re viewing an imported 3MF project—start a new project if you\'d like to add more models.');
    return imported;
  }
  const project = !!getState().project;
  for (const f of list) void Promise.all([import('./api/blobs'), import('./persist/session')])
    .then(([{ blobDigest }, { assignBlobKey }]) => blobDigest(f).then((sum) => assignBlobKey(f, `sha256-${sum}`))).catch(() => {});
  const step = /\.(step|stp|drc)$/i;
  list = list.filter((f) => /\.(stl|obj|3mf)$/i.test(f.name) || step.test(f.name) && (canLoadStep() || /\.drc$/i.test(f.name)) ||
    (toast(`Oops! ${f.name} isn't a supported 3D model. Please pick an STL, OBJ${canLoadStep() ? ', STEP' : ''}, or 3MF file.`), false));
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
      const message = String(e instanceof Error ? e.message : e);
      toast(message.startsWith('Oops') ? message : `Oops! Couldn't load ${f.name}: ${message}`);
    } finally {
      busy?.end();
    }
  }
  if (!task.current()) { for (const o of added) o.geometry.dispose(); return imported; }
  if (!added.length) { task.done(); return imported; }
  const simplified = added.flatMap((o) => o.simplification ? [o.simplification] : []);
  if (simplified.length) void import('./simplify/ui').then((m) => m.noteSimplified(simplified));
  undoable(added.length > 1 ? 'Add objects' : 'Add object', () => setState((s) => {
    const objects = project ? added.reduce((list, o) => {
      const spot = nextFreeSpot(list, o, bedArea()) ?? bedBounds(bedArea()).center;
      return [...list, { ...o, instances: [{ ...o.instances[0], x: spot.x, y: spot.y }] }];
    }, s.objects) : arrange([...s.objects, ...added], bedArea());
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
  const id = await s.api.submit({ library, overrides: {}, events: [], output: '3mf', files: [{ name: f.name, blob: f }] }, signal);
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
  setState(() => ({ objects: [], selected: null, gizmo: 'none', project: null, projectPlates: [], plateIndex: 1, wipeTowerPos: {}, projectName: null, projectMaterials: null, projectNotice: null, events: [], ...(preserveProfiles ? {} : { projectPresets: null, flush: { matrix: null, multiplier: null } }), processScope: 'global', job: { state: 'idle', pct: 0, stage: '' } }));
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
  return () => toast('Opened', { label: 'Undo', run: () => { const id = projects.previousProject(); if (id) void projects.openProject(id); } });
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
  input.accept = ['stl', 'obj', '3mf', ...(canLoadStep() ? ['step', 'stp'] : [])].flatMap((e) => [`.${e}`, `.${e.toUpperCase()}`]).join(',');
  input.onchange = () => input.files && void onFiles(input.files);
  input.click();
}

export function arrangeAll(allPlates = true) {
  const s = getState();
  const locked = !!s.projectPlates.find((p) => p.index === s.plateIndex)?.locked;
  if (locked && !allPlates) return toast('This plate is locked');
  if (s.features.arrange && !s.project && s.api && !s.api.mock) return locked ? toast('This plate is locked') : void import('./plater/operations').then((m) => m.arrangeNative());
  const area = bedArea();
  undoable('Arrange', () => setState((st) => ({
    ...(locked ? {} : { objects: arrange(st.objects, area) }),
    ...(allPlates && !st.project ? { projectPlates: st.projectPlates.map((p) => (p.index === st.plateIndex || p.locked || !p.objects.length ? p : { ...p, objects: arrange(p.objects, area) })) } : {}),
  })));
}

export function addInstance() {
  const s = getState();
  const o = s.objects.find((ob) => ob.id === s.selected?.object);
  if (!o || o.project && (o.project.placeholder || !canEditProject())) return;
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
  if (!o || o.project && (o.project.placeholder || !canEditProject())) return;
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
  if (!o || o.project && (o.project.placeholder || !canEditProject())) return;
  const area = bedArea();
  const src = o.instances[o.instances.length - 1];
  const withCopies = (n: number) => arrange(s.objects.map((ob) => ob.id !== o.id ? ob : { ...ob, instances: [...ob.instances,
    ...Array.from({ length: n }, () => ({ id: newId(), x: src.x, y: src.y, rotZ: src.rotZ, z: src.z, printable: src.printable }))] }), area);
  const fits = (objects: PlateObject[]) => outsideBed(objects, area, Infinity).size === 0;
  let lo = 0, hi = 1;
  let best = withCopies(0);
  if (!fits(best)) return toast('The plate is full—there\'s no room for another copy!');
  for (let next = withCopies(hi); hi <= 1000 && fits(next); next = withCopies(hi)) { best = next; lo = hi; hi *= 2; }
  while (hi - lo > 1) {
    const mid = (lo + hi) >> 1, next = withCopies(mid);
    if (fits(next)) { best = next; lo = mid; } else hi = mid;
  }
  if (!lo) return toast('The plate is full—couldn\'t fit any more copies!');
  undoable('Arrange', () => setState((st) => ({ objects: best, selected: { object: o.id, instance: o.instances[0].id, allInstances: true }, job: markStale(st.job) })));
}

export { outputKind } from './slice-input';
export { confirmDialog, numberDialog, promptDialog };

export async function jobRequest(s = getState(), saving = false): Promise<JobRequest> {
  await flushReady();
  if (!saving) {
    if(s.profileLibraryLoad.state!=='ready')throw new Error(s.profileLibraryLoad.state==='failed'?s.profileLibraryLoad.error:'Hang tight—finishing profile setup before slicing.');
    const {flushPrinterSetups}=await import('./profiles/printer-setups');await flushPrinterSetups();
    validateProfileSubmission(s);
    const [{methodAvailability},{methodSelectionProblem}]=await Promise.all([import('./profiles/method-context'),import('./profiles/capabilities')]);
    const availability=methodAvailability(s), global=effectiveConfig(s,'process');
    const effective=[global,...s.objects.flatMap(o=>[ {...global,...o.settings},...(o.volumes ?? []).map(v=>({...global,...o.settings,...v.settings})),...(o.ranges ?? []).flatMap(r=>(o.volumes?.filter(v=>v.type === 'part') ?? [undefined]).map(v=>({...global,...o.settings,...v?.settings,...r.settings})))])];
    for(const config of effective) {const problem=methodSelectionProblem(availability,config);if(problem) throw new Error(`That slicing method isn't available right now: ${problem.reason}`);}
    const {readProfileLibrary}=await import('./persist/profile-library'), remote=await readProfileLibrary(s.api?.mock);
    for(const [type,key] of [['machine',s.printer],['process',s.process],...s.filamentSlots.map(key=>['filament',key])] as [PresetType,string][]) {
      const local=findProfile(s,type,key), persisted=findProfile({userPresets:remote.profiles.map(normalizeUserPreset)},type,key);
      if(!local || !persisted || (local.revision ?? 0)!==(persisted.revision ?? 0)) throw new Error('Heads up: one of your selected profiles was updated elsewhere. Reload it or save your current edits before slicing.');
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
  const locks = !!s.features.plate_locks;
  if (saving && s.features.mesh_plates && (s.projectPlates.length > 1 || s.projectPlates.some((p) => Object.keys(p.settings).length || p.name || locks && p.locked))) {
    const plates = currentPlates(s);
    const { plate, files } = await plateJson(plates.flatMap((p) => p.objects), firstLayer(s));
    plates.flatMap((p) => p.objects.map(() => p.index)).forEach((index, k) => { plate.objects[k].plate = index; });
    plate.plates = plates.map((p) => ({ index: p.index, name: p.name, settings: Object.fromEntries(Object.entries(p.settings).map(([k, v]) => [k, String(v)])), ...(locks && p.locked ? { locked: true } : {}) }));
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
    library: JSON.stringify({ ...r.library, project: r.overrides }), plate: r.plate && JSON.stringify(r.plate), project_instances: r.project_instances,
    events: r.events.length ? JSON.stringify(r.events) : undefined, output: r.output,
  };
};

let sliceGeneration = 0;

export function outsideVolumeWarning(s = getState()): string | null {
  const machine = printerPreset(s);
  if (!machine) return null;
  const objects = s.objects.filter((o) => !o.project?.placeholder);
  if (!objects.length) return null;
  const outside = outsideBed(objects, parsePoints(machine.config.printable_area), Number(machine.config.printable_height ?? 1e6));
  const names = objects.filter((o) => o.instances.some((i) => outside.has(i.id))).map((o) => o.name);
  return names.length ? `Oops! These objects stick out past the edge of the plate or exceed the max print height: ${names.join('\n')}. Move them fully onto the bed and check their height before slicing.
` : null;
}

function recordWarnings(record: Record<string, unknown> | undefined, stderr: string[]): string[] {
  const text = (record?.text_summary ?? {}) as Record<string, unknown>;
  const objects = Array.isArray(record?.object_warnings) ? record.object_warnings : [];
  const exported = typeof text.export_warning === 'string' ? text.export_warning.split('\n') : [];
  const firstLine = (w: string) => w.trim().split('\n')[0];
  const shown = new Set(stderr.map(firstLine));
  const added: string[] = [];
  for (const w of [...objects, ...exported]) {
    if (typeof w !== 'string' || !w.trim() || shown.has(firstLine(w))) continue;
    shown.add(firstLine(w));
    added.push(w.trim());
  }
  return [...stderr, ...added];
}

export interface JobSink {
  progress(p: { pct: number; stage: string; id?: string }): void;
  done(job: JobView): void;
  failed(error: string): void;
  timeoutWarning?(): void;
}

export function runJob(api: Api, request: JobRequest, signature: string, sink: JobSink, calib?: string): () => void {
  let live = true, id: string | undefined, stop: (() => void) | undefined;
  const submission = new AbortController();
  void (async () => {
    try {
      id = await api.submit(request, submission.signal, (fraction) => live && sink.progress({ pct: Math.round(fraction * 100), stage: 'uploading' }));
      if (!live) return void api.cancel(id).catch(() => undefined);
      sink.progress({ id, pct: 0, stage: 'queued' });
      const warnings: string[] = [], calibLog: string[] = [];
      let pct = 0, resulted = false;
      const complete = (status: JobStatus) => sink.done({ id: id!, state: 'done', pct: 100, stage: 'done', status, plateSignature: signature, warnings: recordWarnings(status.record, warnings), ...(calib ? { calib, calibLog } : {}) });
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
        timeoutWarning: () => { if (live) sink.timeoutWarning?.(); },
        error: (msg) => {
          if (!live) return;
          live = false;
          sink.failed(msg);
        },
        log: (stream, line) => {
          if (stream === 'stdout' && line.startsWith('calib_')) calibLog.push(line);
          if (stream !== 'stderr' || !line.trim()) return;
          const m = /^warning:\s*(.*)$/i.exec(line);
          if (m) warnings.push(m[1]);
          else if (warnings.length && !/^error:/i.test(line)) warnings[warnings.length - 1] += `\n${line}`;
        },
      });
    } catch (e) {
      if (!live) return;
      live = false;
      sink.failed(String(e));
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
  const outside = outsideVolumeWarning(s);
  if (outside) return outside;
  if (nativeSupportCapable(s) && (String(effectiveConfig(s, 'process').support_style) === 'native' || s.objects.some((o) => o.settings.support_style === 'native'))) {
    const conflicts = (await import('./plater/nativeSupport')).nativeSupportConflicts(s);
    if (conflicts && !ask) return conflicts;
    if (conflicts && !(await confirmDialog('Native support', `${conflicts} Would you like to slice anyway?`))) return null;
  }
  return jobRequest(s);
}

let cancelJob: (() => void) | null = null;

export async function slice(calib?: { kind: string; params: string; head?: string }) {
  await flushReady();
  const [{methodAvailability}, {methodSelectionProblem}] = await Promise.all([import('./profiles/method-context'), import('./profiles/capabilities')]);
  const s = getState();
  if (!s.api || !s.objects.length && !calib || s.job.state === 'running' || s.job.state === 'submitting') return;
  const methodProblem = methodSelectionProblem(methodAvailability(s), effectiveConfig(s,'process'));
  if(methodProblem) return void setState({job:{state:'failed',pct:0,stage:'',error:`That slicing method isn't available right now: ${methodProblem.reason}`}});
  const generation = ++sliceGeneration, document = documentGeneration(), projectId = projectFlow.get().context.id, plate = s.plateIndex;
  const current = () => generation === sliceGeneration && document === documentGeneration() && projectId === projectFlow.get().context.id && plate === getState().plateIndex;
  const signature = plateSignature(s);
  const { library, output } = jobFields(s);
  const request = calib ? { library, overrides: calibOverrides(s, calib.kind), output, events: [], files: [], calib: calib.kind, calib_params: calib.params, calib_head: calib.head }
    : await plateJob(s).catch((e: unknown) => String(e));
  if (!current() || request === null) return;
  if (typeof request === 'string') return void setState({ job: { state: 'failed', pct: 0, stage: '', error: request } });
  const navigation = beginSlicePreview(signature);
  void loadPreviewView();
  setState({ tab: 'prepare', job: { state: 'submitting', pct: 0, stage: 'uploading', plateSignature: signature, calib: calib?.kind } });
  cancelJob = runJob(s.api, request, signature, {
    progress: ({ id, ...p }) => { if (!current()) return; bindSlicePreview(navigation, id); setState(st => id ? {job: {...st.job, id, state: 'running', ...p}} : st.job.state === 'submitting' ? {job: {...st.job, ...p}} : {}); },
    done: (job) => { if (!current()) return; bindSlicePreview(navigation, job.id); setState({job: {...job, stale: plateSignature(getState()) !== signature}}); },
    failed: (error) => { if (current()) setState(st => ({job: {...st.job, state: 'failed', error}})); },
    timeoutWarning: () => {
      if (current()) void confirmDialog('Slicing is taking longer than expected', 'The slice is still running. It will continue automatically unless you stop it.', ['Continue', 'Stop']).then((ok) => { if (!ok) cancelSlice(); });
    },
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
    ...effectiveConfig(s, 'machine'), ...effectiveConfig(s, 'filament'), ...effectiveConfig(s, 'process'), ...overrides(s),
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

function exportBlocker(s: State) {
  if (!s.objects.length) return 'Nothing to export yet! Add a model to the plate and slice it first.';
  const outside = outsideVolumeWarning(s);
  if (outside) return outside;
  if (s.job.state === 'running' || s.job.state === 'submitting') return 'Still slicing! You\'ll be able to export as soon as it finishes.';
  if (s.job.state === 'done' && !s.job.stale && curveRefused(s)) return 'Oops! We couldn\'t slice this model with curved layers. Try slicing with flat layers instead, or tweak your curved-layer settings.';
  return s.job.state === 'done' ? 'You\'ve made changes since the last slice—slice the plate again to update the toolpaths.' : 'Slice the plate first before continuing.';
}

export async function download() {
  if (import.meta.env.MODE === 'demo') return void toast('Not available in demo mode', null, 3500, 'warning');
  const s = getState();
  if (!s.api || !canExport(s) || !s.job.id) return void (s.api && toast(exportBlocker(s)));
  const task = output.begin('exporting');
  let href: string;
  try {
    const r = await fetch(s.api.downloadUrl(s.job.id), { signal: task.signal });
    if (!r.ok) throw r.status === 409 ? await responseError(r) : new Error(`${r.status} ${r.statusText}`);
    href = URL.createObjectURL(await r.blob());
  } catch (e) {
    if (!task.current()) return;
    task.fail(e);
    toast((e as ApiError).code === 'could_not_curve' ? (e as ApiError).message : `Oops! Export failed: ${e instanceof Error ? e.message : e}`);
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
  if (import.meta.env.MODE === 'demo') return void toast('Not available in demo mode', null, 3500, 'warning');
  const saving = await import('./saveProject');
  return saving.saveProject(as);
}

export function toggleText() {
  const s = getState();
  if (!s.features.emboss_text || s.tab !== 'prepare') return;
  if (s.gizmo === 'text') return setState({ gizmo: 'none' });
  void import('./plater/text').then((m) => {
    if (m.editedText()) return setState({ gizmo: 'text' });
    const o = s.objects.find((x) => x.id === s.selected?.object);
    if (o && (o.project?.placeholder || o.native || s.selected?.all)) return toast('Select one object on the plate (or click empty space to deselect all) to add text.');
    void m.addText(o ? o.id : null);
  });
}

export const canBoolean = (s: State) => {
  const o = s.objects.find((x) => x.id === s.selected?.object);
  return !!s.features.mesh_boolean && !!o && !s.selected?.all && !o.project?.placeholder && !o.native && !!o.volumes?.length;
};
export function toggleBoolean() {
  const s = getState();
  if (s.gizmo === 'boolean' || canBoolean(s)) setState({ gizmo: s.gizmo === 'boolean' ? 'none' : 'boolean' });
}

export function toggleSvg() {
  const s = getState();
  if (!s.features.emboss_svg || s.tab !== 'prepare') return;
  if (s.gizmo === 'svg') return setState({ gizmo: 'none' });
  void import('./plater/svg').then((m) => {
    if (m.editedSvg()) return setState({ gizmo: 'svg' });
    const o = s.objects.find((x) => x.id === s.selected?.object);
    if (o && (o.project?.placeholder || o.native || s.selected?.all)) return toast('Select one object on the plate (or click empty space to deselect all) to add an SVG.');
    void m.addSvg(o ? o.id : null);
  });
}
