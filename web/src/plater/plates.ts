import { IDLE_JOB, getState, markStale, newId, setState, subscribeStore, toast, type JobView, type PlateObject, type State } from '../state/store';
import { undoable } from '../state/history';
import { bedArea, canExport, exportFilename, slice } from '../actions';
import { arrange, nextFreeSpot } from './geometry';
import { promptDialog } from '../components/Ask';

export const OBJECT_DRAG = 'application/x-orca-object';


const plateJobs = new Map<number, JobView>();

const busy = (s: State) => s.job.state === 'running' || s.job.state === 'submitting';

export const canAddPlate = (s: State) => !s.project;

export function currentPlates(s: State) {
  return s.projectPlates.map((p) => (p.index === s.plateIndex ? { ...p, objects: s.objects, events: s.events } : p));
}

export const listedPlates = (s: State) =>
  s.projectPlates.length ? currentPlates(s) : [{ index: 1, name: '', origin: [0, 0] as [number, number], settings: {}, objects: s.objects, events: s.events }];

export const plateLocked = (s: State, index = s.plateIndex) => !!s.projectPlates.find((p) => p.index === index)?.locked;

function showPlate(s: State, index: number): Partial<State> {
  const plates = currentPlates(s);
  const target = plates.find((p) => p.index === index)!;
  if (s.job.state === 'done' || s.job.state === 'failed') plateJobs.set(s.plateIndex, s.job);
  const job = plateJobs.get(index) ?? IDLE_JOB;
  plateJobs.delete(index);
  return { projectPlates: plates, objects: target.objects, events: target.events ?? [], plateIndex: index, selected: null, gizmo: 'none', job };
}

export function selectPlate(index: number) {
  const s = getState();
  if (index === s.plateIndex || busy(s)) return;
  undoable('Select plate', () => setState((st) => showPlate(st, index)));
}

export function addPlate() {
  const s = getState();
  if (!canAddPlate(s) || busy(s)) return;
  undoable('Add Plate', () => setState((st) => {
    const plates = listedPlates(st);
    const index = Math.max(...plates.map((p) => p.index)) + 1;
    return showPlate({ ...st, projectPlates: [...plates, { index, name: '', origin: [0, 0], settings: {}, objects: [], events: [] }] }, index);
  }));
}

export function duplicatePlate() {
  const s = getState();
  if (!canAddPlate(s) || busy(s)) return;
  undoable('Duplicate Current Plate', () => setState((st) => {
    const plates = listedPlates(st);
    const index = Math.max(...plates.map((p) => p.index)) + 1;
    const objects = st.objects.map((o) => ({ ...o, id: newId(), settings: { ...o.settings }, instances: o.instances.map((i) => ({ ...i, id: newId() })) }));
    return showPlate({ ...st, projectPlates: [...plates, { index, name: '', origin: [0, 0], settings: {}, objects, events: [] }] }, index);
  }));
}

export function deletePlate(index = getState().plateIndex) {
  const s = getState();
  if (!canAddPlate(s) || s.projectPlates.length < 2 || busy(s)) return;
  undoable('Delete Plate', () => setState((st) => {
    const left = currentPlates(st).filter((p) => p.index !== index).map((p, i) => ({ ...p, index: i + 1 }));
    const after = (k: number) => (k > index ? k - 1 : k);
    const jobs = new Map(plateJobs);
    if (st.plateIndex !== index) jobs.set(st.plateIndex, st.job);
    plateJobs.clear();
    for (const [k, j] of jobs) if (k !== index) plateJobs.set(after(k), j);
    const selected = st.plateIndex === index ? Math.min(index, left.length) : after(st.plateIndex);
    const job = plateJobs.get(selected) ?? IDLE_JOB;
    plateJobs.delete(selected);
    const target = left[selected - 1];
    return {
      projectPlates: left.length > 1 ? left : [], plateIndex: left.length > 1 ? selected : 1,
      objects: target.objects, events: target.events ?? [], selected: null, gizmo: 'none', job,
    };
  }));
}

export const PLATE_KEYS = ['curr_bed_type', 'print_sequence', 'spiral_mode', 'first_layer_print_sequence', 'other_layers_print_sequence', 'other_layers_print_sequence_nums'] as const;

export function setPlateSettings(name: string, settings: Record<string, string>) {
  undoable('Plate Settings', () => setState((st) => {
    const plates = listedPlates(st);
    return {
      projectPlates: plates.map((p) => (p.index === st.plateIndex ? { ...p, name, settings: { ...Object.fromEntries(Object.entries(p.settings).filter(([k]) => !(PLATE_KEYS as readonly string[]).includes(k))), ...settings } } : p)),
      job: markStale(st.job),
    };
  }));
}

export function toggleLock(index: number) {
  undoable(plateLocked(getState(), index) ? 'Unlock Plate' : 'Lock Plate', () => setState((st) => ({
    projectPlates: listedPlates(st).map((p) => (p.index === index ? { ...p, locked: !p.locked } : p)),
  })));
}

export function movePlateToFront(index: number) {
  const s = getState();
  if (index <= 1 || s.projectPlates.length < 2 || busy(s)) return;
  undoable('Move plate to front', () => setState((st) => {
    const plates = currentPlates(st);
    const renumber = (k: number) => (k === index ? 1 : k < index ? k + 1 : k);
    const jobs = new Map(plateJobs);
    jobs.set(st.plateIndex, st.job);
    plateJobs.clear();
    for (const [k, j] of jobs) plateJobs.set(renumber(k), j);
    const selected = renumber(st.plateIndex);
    const job = plateJobs.get(selected) ?? IDLE_JOB;
    plateJobs.delete(selected);
    const wipeTowerPos = Object.fromEntries(Object.entries(st.wipeTowerPos).map(([k, v]) => [renumber(Number(k)), v]));
    return {
      projectPlates: plates.map((p) => ({ ...p, index: renumber(p.index) })).sort((a, b) => a.index - b.index),
      plateIndex: selected, job, wipeTowerPos,
    };
  }));
}

export function renamePlate(index: number, name: string) {
  undoable('Edit Plate Name', () => setState((st) => ({ projectPlates: listedPlates(st).map((p) => (p.index === index ? { ...p, name } : p)) })));
}

export async function editPlateName() {
  const s = getState();
  const plate = listedPlates(s).find((p) => p.index === s.plateIndex);
  if (!plate) return;
  const name = await promptDialog('Edit Plate Name', plate.name);
  if (name !== null) renamePlate(plate.index, name);
}

export function moveToPlate(ids: number[], index: number, shift?: [number, number]) {
  const s = getState();
  if (index === s.plateIndex || !ids.length || plateLocked(s, index)) return;
  undoable(ids.length > 1 ? 'Move objects to plate' : 'Move object to plate', () => setState((st) => {
    const moving = st.objects.filter((o) => ids.includes(o.id));
    const area = bedArea();
    const target = st.projectPlates.find((p) => p.index === index)!;
    let placed = target.objects;
    for (const o of moving) {
      if (shift) { placed = [...placed, shiftTo(o, o.instances[0].x - shift[0], o.instances[0].y - shift[1])]; continue; }
      const spot = placed.length ? nextFreeSpot(placed, o, area) : null;
      placed = spot ? [...placed, shiftTo(o, spot.x, spot.y)] : arrange([...placed, o], area);
    }
    return {
      objects: st.objects.filter((o) => !ids.includes(o.id)),
      projectPlates: st.projectPlates.map((p) => (p.index === index ? { ...p, objects: placed } : p)),
      selected: null, job: markStale(st.job),
    };
  }));
}

function shiftTo(o: PlateObject, x: number, y: number): PlateObject {
  const dx = x - o.instances[0].x, dy = y - o.instances[0].y;
  return { ...o, instances: o.instances.map((i) => ({ ...i, x: i.x + dx, y: i.y + dy })) };
}

export const plateJob = (s: State, index: number) => (index === s.plateIndex ? s.job : plateJobs.get(index) ?? IDLE_JOB);

export function setPlateJob(index: number, job: JobView) {
  if (index === getState().plateIndex) setState({ job });
  else plateJobs.set(index, job);
}

export const clearPlateJobs = () => plateJobs.clear();

function asPlate(s: State, index: number): State {
  const plate = currentPlates(s).find((p) => p.index === index)!;
  return { ...s, projectPlates: currentPlates(s), objects: plate.objects, events: plate.events ?? [], plateIndex: index, job: plateJob(s, index) };
}

const settled = () => new Promise<void>((resolve) => {
  const check = () => { if (!busy(getState())) { stop(); resolve(); } };
  const stop = subscribeStore(check);
  check();
});

export async function sliceAll() {
  const start = getState();
  if (busy(start) || start.projectPlates.length < 2) return;
  for (const { index } of currentPlates(start)) {
    const s = getState();
    if (!asPlate(s, index).objects.length || canExport(asPlate(s, index))) continue;
    if (s.plateIndex !== index) setState(showPlate(s, index));
    await slice();
    await settled();
    if (getState().job.state !== 'done') return;
  }
  if (getState().plateIndex !== start.plateIndex) setState(showPlate(getState(), start.plateIndex));
}

export const exportablePlates = (s: State) => s.projectPlates.filter((p) => canExport(asPlate(s, p.index)));

export async function exportAll() {
  const s = getState();
  const plates = exportablePlates(s);
  if (!s.api || !plates.length) return;
  try {
    const { zipSync } = await import('fflate');
    const files: Record<string, [Uint8Array, { level: 0 }]> = {};
    for (const p of plates) {
      const state = asPlate(s, p.index);
      const r = await fetch(s.api.downloadUrl(state.job.id!));
      if (!r.ok) throw new Error(`${r.status} ${r.statusText}`);
      files[exportFilename(state).replace(/(\.gcode(\.3mf)?)$/, `_plate_${p.index}$1`)] = [new Uint8Array(await r.arrayBuffer()), { level: 0 }];
    }
    const name = (s.projectName ?? s.project?.name ?? currentPlates(s).flatMap((p) => p.objects)[0]?.name ?? 'plates').replace(/\.(stl|obj|3mf)$/i, '');
    const href = URL.createObjectURL(new Blob([zipSync(files)], { type: 'application/zip' }));
    const a = document.createElement('a');
    a.href = href; a.download = `${name}.zip`; document.body.appendChild(a); a.click(); a.remove();
    setTimeout(() => URL.revokeObjectURL(href), 60000);
  } catch (e) {
    toast('export_failed', { detail: e instanceof Error ? e.message : e });
  }
}
