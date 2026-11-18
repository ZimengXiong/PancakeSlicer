import * as THREE from 'three';
import { responseError } from '../api/access';
import { effectiveConfig, getState, setState, subscribeStore, type State, type WipeTowerEstimate } from '../state/store';
import { plateSignature } from '../slice-input';
import { plateForm } from './operations';
import { postJob } from '../api/blobs';
import { slotFilaments } from '../profiles/slots';

function wanted(s: State): boolean {
  if (!s.api || s.api.mock || s.project || !s.objects.length) return false;
  const c = effectiveConfig(s, 'process');
  const on = (v: unknown) => v === true || v === 1 || v === '1' || v === 'true';
  if (!on(c.enable_prime_tower) || on(effectiveConfig(s, 'machine').belt_printer)) return false;
  if (c.print_sequence === 'by object' && s.objects.flatMap((o) => o.instances).filter((i) => i.printable !== false).length !== 1) return false;
  return slotFilaments(s).length > 1 || String(c.timelapse_type) === '1' || on(c.enable_wrapping_detection);
}

const round = (v: number) => Math.round(v * 100) / 100;

let requested = '';
let timer = 0;
let running = false;

async function estimate() {
  const s = getState();
  if (!wanted(s)) {
    requested = '';
    if (s.wipeTower) setState({ wipeTower: null });
    return;
  }
  const key = plateSignature(s);
  if (key === requested) return;
  if (running) { schedule(); return; }
  requested = key;
  running = true;
  try {
    const fd = await plateForm(s, 'wipe-tower');
    fd.append('priority', 'low');
    const response = await postJob(fd, '/api/jobs', { quiet: true });
    if (!response.ok) throw await responseError(response);
    const { id } = await response.json() as { id: string };
    try {
      const record = await new Promise<Record<string, unknown> | undefined>((resolve, reject) => {
        const stop = s.api!.events(id, { progress() {}, done(st) { stop(); resolve(st.record); }, error(m) { stop(); reject(new Error(m)); } });
      });
      const plate = (record?.plates as WipeTowerEstimate[] | undefined)?.[0];
      if (requested === key && record?.schema === 'orca.wipe_tower.v1') {
        const tower = plate && plate.depth > 0 && plate.width > 0 ? plate : null;
        const at = getState().wipeTowerPos[getState().plateIndex];
        const clamped = tower && at && (Math.abs(at[0] - tower.x) > 0.01 || Math.abs(at[1] - tower.y) > 0.01);
        setState((st) => ({ wipeTower: tower, ...(clamped ? { wipeTowerPos: { ...st.wipeTowerPos, [st.plateIndex]: [round(tower.x), round(tower.y)] } } : {}) }));
      }
    } finally {
      await s.api!.cancel(id);
    }
  } catch {
    requested = '';
    setState({ wipeTower: null });
  } finally {
    running = false;
  }
}

function schedule() {
  clearTimeout(timer);
  timer = window.setTimeout(() => void estimate(), 400);
}

let started = false;
export function startWipeTower() {
  if (started) return;
  started = true;
  let last: State | null = null;
  subscribeStore(() => {
    const s = getState();
    if (last && s.objects === last.objects && slotFilaments(s) === slotFilaments(last) && s.dirty === last.dirty && s.printer === last.printer &&
      s.process === last.process && s.wipeTowerPos === last.wipeTowerPos && s.filamentEdits === last.filamentEdits && s.plateIndex === last.plateIndex &&
      s.project === last.project) return;
    last = s;
    schedule();
  });
  schedule();
}

export class TowerView {
  readonly group = new THREE.Group();
  private body = new THREE.Mesh(new THREE.BoxGeometry(1, 1, 1), new THREE.MeshStandardMaterial({ color: 0x8a9a5b, transparent: true, opacity: 0.6, depthWrite: false }));
  private brim = new THREE.Mesh(new THREE.PlaneGeometry(1, 1), new THREE.MeshBasicMaterial({ color: 0x8a9a5b, transparent: true, opacity: 0.35, depthWrite: false, side: THREE.DoubleSide }));
  dragAt: [number, number] | null = null;

  constructor() {
    this.body.userData.wipeTower = true;
    this.group.add(this.brim, this.body);
    this.group.visible = false;
  }

  sync(s: State) {
    const t = s.wipeTower;
    this.group.visible = !!t && !s.project;
    if (!t) return;
    const [x, y] = this.dragAt ?? s.wipeTowerPos[s.plateIndex] ?? [t.x, t.y];
    this.body.scale.set(t.width, t.depth, Math.max(t.height, 0.2));
    this.body.position.set(x + t.width / 2, y + t.depth / 2, Math.max(t.height, 0.2) / 2);
    this.brim.scale.set(t.width + 2 * t.brim_width, t.depth + 2 * t.brim_width, 1);
    this.brim.position.set(x + t.width / 2, y + t.depth / 2, 0.05);
  }

  hit(ray: THREE.Raycaster): THREE.Intersection | undefined {
    return this.group.visible ? ray.intersectObject(this.body, false)[0] : undefined;
  }

  static inBed(at: [number, number], t: WipeTowerEstimate, area: { x: number; y: number }[]): [number, number] {
    const xs = area.map((p) => p.x), ys = area.map((p) => p.y), b = t.brim_width;
    const clamp = (v: number, lo: number, hi: number) => (hi < lo ? v : Math.min(Math.max(v, lo), hi));
    return [round(clamp(at[0], Math.min(...xs) + b, Math.max(...xs) - b - t.width)), round(clamp(at[1], Math.min(...ys) + b, Math.max(...ys) - b - t.depth))];
  }

  corner(s: State): [number, number] | null {
    const t = s.wipeTower;
    return t ? this.dragAt ?? s.wipeTowerPos[s.plateIndex] ?? [t.x, t.y] : null;
  }
}
