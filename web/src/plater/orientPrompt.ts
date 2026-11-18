import { objectValues } from '../profiles/view';
import { firstLayer, jobFields } from '../slice-input';
import { loadMoves, supportGrams } from '../preview/data';
import { getState, printerPreset, setState, subscribeStore, type OrientPrompt, type OrientSlice, type PlateObject, type State } from '../state/store';
import { undoable } from '../state/history';
import { instanceBox, parsePoints, plateJson } from './geometry';
import { orientJob } from './operations';
import { axisAngle, withRotation } from './rotation';

interface Score { object?: unknown; original?: unknown; best?: unknown; axis?: unknown; angle_deg?: unknown; support_area?: unknown; float_area?: unknown }

const num = (v: unknown) => (typeof v === 'number' && Number.isFinite(v) ? v : undefined);
const pose = (o: PlateObject) => JSON.stringify([o.rotX, o.rotY, o.scale]);

function fits(o: PlateObject, rotation: number[]) {
  const machine = printerPreset(getState());
  if (!machine) return false;
  const area = parsePoints(machine.config.printable_area), xs = area.map((p) => p.x), ys = area.map((p) => p.y);
  const turned = withRotation(o, rotation);
  const box = instanceBox(turned, turned.instances[0]);
  return box.max.x - box.min.x <= Math.max(...xs) - Math.min(...xs) && box.max.y - box.min.y <= Math.max(...ys) - Math.min(...ys) &&
    box.max.z - box.min.z < Number(machine.config.printable_height ?? 1e6) - 0.01;
}

function decide(o: PlateObject, scores: Score[]): OrientPrompt | null {
  const mine = scores.filter((x) => (num(x.object) ?? 0) === 0);
  const original = mine.find((x) => x.original === true);
  const support = num(original?.support_area), floating = num(original?.float_area);
  if (support === undefined || floating === undefined || support <= 0) return null;
  const others = mine.filter((x) => x.original === false && num(x.support_area) !== undefined && num(x.float_area) !== undefined);
  const best = others.find((x) => x.best === true) ?? [...others].sort((a, b) => num(a.support_area)! - num(b.support_area)!)[0];
  const axis = best?.axis as number[], angle = num(best?.angle_deg);
  if (!best || !Array.isArray(axis) || axis.length !== 3 || !axis.every(Number.isFinite) || angle === undefined || Math.abs(angle) < 1) return null;
  if (num(best.support_area)! > 0.5 * support || num(best.float_area)! > floating) return null;
  const rotation = axisAngle(axis, angle);
  return fits(o, rotation) ? { rotation, pose: pose(o), supportArea: [support, num(best.support_area)!] } : null;
}

const without = (st: State, id: number) => {
  const next = { ...st.orientPrompts };
  delete next[id];
  return next;
};

function fromProject(o: PlateObject) {
  const n = o.native;
  return !!n && o.source === n.files[String((n.entry.volumes as { file: string }[])[n.main].file)];
}

const scored = new Map<number, string>();
let queue = Promise.resolve();

async function score(o: PlateObject, printer: string) {
  let scores: Score[] = [];
  try {
    const record = await orientJob(o, 'orient-scores');
    scores = Array.isArray(record?.scores) ? (record.scores as Score[]) : [];
  } catch {
    return;
  }
  const s = getState(), now = s.objects.find((x) => x.id === o.id);
  if (!now || s.printer !== printer || pose(now) !== pose(o) || s.orientDismissed.includes(o.id)) return;
  const prompt = decide(now, scores);
  setState((st) => ({ orientPrompts: prompt ? { ...st.orientPrompts, [o.id]: prompt } : without(st, o.id) }));
}

function check() {
  const s = getState();
  if (!s.api || s.api.mock || s.project) return;
  for (const o of s.objects) {
    const key = `${s.printer}|${pose(o)}`;
    if (o.project?.placeholder || fromProject(o) || s.orientDismissed.includes(o.id) || scored.get(o.id) === key) continue;
    const cfg = objectValues(s, o.id).values as Record<string, unknown>;
    const on = (v: unknown) => v === true || v === 1 || v === '1' || v === 'true';
    const mode = String(Array.isArray(cfg.nonplanar_mode) ? cfg.nonplanar_mode[0] : cfg.nonplanar_mode ?? 'off');
    if (!on(cfg.enable_support) || mode !== 'off') {
      if (s.orientPrompts[o.id]) setState((st) => ({ orientPrompts: without(st, o.id) }));
      continue;
    }
    scored.set(o.id, key);
    if (s.orientPrompts[o.id] && s.orientPrompts[o.id].pose !== pose(o)) setState((st) => ({ orientPrompts: without(st, o.id) }));
    const printer = s.printer;
    queue = queue.then(() => score(o, printer));
  }
}

let started = false;
export function startOrientPrompt() {
  if (started) return;
  started = true;
  subscribeStore(check);
  check();
}

async function sliceAlone(o: PlateObject): Promise<OrientSlice> {
  const s = getState();
  const area = parsePoints(printerPreset(s)?.config.printable_area), xs = area.map((p) => p.x), ys = area.map((p) => p.y);
  const alone = { ...o, instances: [{ ...o.instances[0], z: 0 }] };
  const box = instanceBox(alone, alone.instances[0]);
  const inst = alone.instances[0];
  alone.instances = [{ ...inst, x: inst.x + (Math.min(...xs) + Math.max(...xs)) / 2 - (box.min.x + box.max.x) / 2,
    y: inst.y + (Math.min(...ys) + Math.max(...ys)) / 2 - (box.min.y + box.max.y) / 2 }];
  const { plate, files } = await plateJson([alone], firstLayer(s));
  const id = await s.api!.submit({ ...jobFields(s), plate, files, output: 'gcode', priority: 'low', floating: true });
  try {
    const status = await new Promise<{ record?: Record<string, unknown> }>((resolve, reject) => {
      const stop = s.api!.events(id, { progress() {}, done(st) { stop(); resolve(st); }, error(m) { stop(); reject(new Error(m)); } });
    });
    const record = status.record ?? {};
    const text = (record.text_summary ?? {}) as Record<string, unknown>;
    const weight = num((record.statistics as Record<string, unknown> | undefined)?.total_weight_g) ?? null;
    const data = await new Promise<Parameters<typeof supportGrams>[0]>((resolve) => {
      loadMoves({ url: s.api!.movesUrl(id) }, { header() {}, chunk() {}, done: resolve, error: () => resolve(null) });
    });
    const floating = (record.totals as Record<string, unknown> | undefined)?.floating as Record<string, unknown> | undefined;
    return { time_s: num(Number(text.estimated_time_s)) ?? null, support_g: supportGrams(data, weight), floating_mm: num(floating?.part_mm) ?? null };
  } finally {
    await s.api!.cancel(id);
  }
}

export async function confirmOrientPrompt(id: number) {
  const s = getState(), prompt = s.orientPrompts[id], o = s.objects.find((x) => x.id === id);
  if (!prompt || !o || pose(o) !== prompt.pose || !s.api) {
    setState((st) => ({ orientPrompts: without(st, id) }));
    return;
  }
  const support = objectValues(s, id).values.enable_support;
  const base = { id, supportOn: support === true || support === 1 || support === '1' || support === 'true', supportArea: prompt.supportArea };
  setState({ orientConfirm: base });
  try {
    const original = await sliceAlone(o);
    if (getState().orientConfirm?.id !== id) return;
    setState({ orientConfirm: { ...base, original } });
    const standing = await sliceAlone(withRotation(o, prompt.rotation));
    if (getState().orientConfirm?.id !== id) return;
    setState({ orientConfirm: { ...base, original, standing } });
  } catch (e) {
    if (getState().orientConfirm?.id === id) setState({ orientConfirm: { ...base, error: e instanceof Error ? e.message : String(e) } });
  }
}

export function standUp(id: number) {
  const s = getState(), prompt = s.orientPrompts[id], o = s.objects.find((x) => x.id === id);
  setState((st) => ({ orientConfirm: null, orientPrompts: without(st, id) }));
  if (!prompt || !o || pose(o) !== prompt.pose) return;
  const turned = withRotation(o, prompt.rotation);
  undoable('Orient', () => setState((st) => ({ objects: st.objects.map((x) => (x.id === id ? turned : x)) })));
}
