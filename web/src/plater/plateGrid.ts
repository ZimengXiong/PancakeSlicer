import * as THREE from 'three';
import type { State } from '../state/store';
import { setState } from '../state/store';
import { bedBounds, instanceMatrix } from './geometry';
import { showBedFor } from './Bed';
import { filamentColor } from './filamentColor';
import { canAddPlate, deletePlate, editPlateName, listedPlates, movePlateToFront, PLATE_KEYS, selectPlate, toggleLock } from './plates';
import { arrangeAll, toolbarOperation } from '../actions';
import { isDark } from '../theme';

const GAP = 1 / 5;
const ICON = 16, GAP_LEFT = 3, GAP_Y = 5, GAP_TOP = 3;

export function plateColumns(count: number) {
  const v = Math.sqrt(count), r = Math.round(v);
  return v > r ? r + 1 : r;
}

export function gridOrigin(index: number, count: number, area: THREE.Vector2[]): [number, number] {
  const { min, max } = bedBounds(area), cols = plateColumns(Math.max(1, count)), i = index - 1;
  return [(i % cols) * (max.x - min.x) * (1 + GAP), -Math.floor(i / cols) * (max.y - min.y) * (1 + GAP)];
}

export function plateOffset(s: State, area: THREE.Vector2[], index: number): [number, number] {
  const n = s.projectPlates.length, a = gridOrigin(index, n, area), b = gridOrigin(s.plateIndex, n, area);
  return [a[0] - b[0], a[1] - b[1]];
}

const HOVER: Record<string, string> = {
  plate_close: 'plate_close_hover', plate_orient: 'plate_orient_hover', plate_arrange: 'plate_arrange_hover', plate_locked: 'plate_locked_hover',
  plate_unlocked: 'plate_unlocked_hover', plate_settings: 'plate_settings_hover', plate_settings_changed: 'plate_settings_changed_hover',
  plate_move_front: 'plate_move_front_hover', plate_name_edit: 'plate_name_edit_hover',
};
const icon = (name: string, hover = false) => `/images/${isDark() ? 'dark/' : ''}${hover ? HOVER[name] : name}.svg`;

export class PlateGrid {
  readonly group = new THREE.Group();
  readonly layer = document.createElement('div');
  private key: unknown[] = [];
  private beds: THREE.Object3D[] = [];
  private area: THREE.Vector2[] = [];
  private anchors: { el: HTMLElement; at: THREE.Vector3; size: number; kind: 'icon' | 'number' | 'name' }[] = [];

  constructor(private material: (color: string) => THREE.Material) {
    this.layer.className = 'plate-icons';
  }

  sync(s: State, bed: THREE.Group | null, area: THREE.Vector2[]) {
    const key = [s.projectPlates, s.plateIndex, s.objects.length, bed, s.records, s.project, isDark(), s.job.state];
    if (key.length === this.key.length && key.every((k, i) => k === this.key[i])) return;
    this.key = key;
    this.area = area;
    this.group.clear();
    this.beds = [];
    this.anchors = [];
    this.layer.replaceChildren();
    if (!bed || !area.length) return;
    const plates = listedPlates(s), many = plates.length > 1;
    for (const p of plates) {
      const [dx, dy] = plateOffset(s, area, p.index);
      if (p.index !== s.plateIndex) {
        const g = new THREE.Group();
        g.position.set(dx, dy, 0);
        const b = bed.clone();
        this.beds.push(b);
        g.add(b);
        for (const o of p.objects)
          for (const inst of o.instances) {
            const m = new THREE.Mesh(o.geometry, this.material(inst.printable === false ? '#999999' : filamentColor(s, o.extruder)));
            m.matrixAutoUpdate = false;
            m.matrix.copy(instanceMatrix(o, inst));
            g.add(m);
          }
        g.userData.plate = p.index;
        this.group.add(g);
      }
      this.addIcons(s, p, dx, dy, many);
    }
  }

  private addIcons(s: State, p: ReturnType<typeof listedPlates>[number], dx: number, dy: number, many: boolean) {
    const { min, max } = bedBounds(this.area), w = max.x - min.x, d = max.y - min.y;
    const f = Math.min(d / 200, (w * GAP) / (ICON + 2 * GAP_LEFT)), size = ICON * f;
    const editable = canAddPlate(s), idle = s.job.state !== 'running' && s.job.state !== 'submitting';
    const onPlate = (fn: () => void) => () => { if (p.index !== s.plateIndex) selectPlate(p.index); fn(); };
    const locked = !!p.locked;
    const changed = PLATE_KEYS.some((k) => p.settings[k] !== undefined && p.settings[k] !== '');
    const slots: ([string, string, () => void, boolean] | null)[] = [
      editable && many ? ['plate_close', 'Remove current plate', () => deletePlate(p.index), idle] : null,
      many ? ['plate_orient', 'Auto orient objects on current plate', onPlate(() => void toolbarOperation('orient', true)), !s.project && !locked && p.objects.length > 0] : null,
      many ? ['plate_arrange', 'Arrange objects on current plate', onPlate(() => arrangeAll(false)), !locked && p.objects.length > 0] : null,
      many ? [locked ? 'plate_locked' : 'plate_unlocked', locked ? 'Unlock current plate' : 'Lock current plate', () => toggleLock(p.index), true] : null,
      editable ? [changed ? 'plate_settings_changed' : 'plate_settings', 'Customize current plate', onPlate(() => setState({ appDialog: 'plate' })), idle] : null,
      editable && many ? ['plate_move_front', 'Move plate to the front', () => movePlateToFront(p.index), idle && p.index > 1] : null,
    ];
    slots.filter(Boolean).forEach((slot, i) => {
      if (!slot) return;
      const [name, title, action, enabled] = slot;
      const b = document.createElement('button');
      b.type = 'button';
      b.className = 'plate-icon';
      b.title = title;
      b.setAttribute('aria-label', `${title} (plate ${p.index})`);
      b.disabled = !enabled;
      const img = document.createElement('img');
      img.src = icon(name);
      img.alt = '';
      img.draggable = false;
      b.append(img);
      b.onmouseenter = () => { if (!b.disabled) img.src = icon(name, true); };
      b.onmouseleave = () => { img.src = icon(name); };
      b.onclick = (e) => { e.stopPropagation(); action(); };
      const top = max.y - (i * (ICON + GAP_Y) + GAP_TOP) * f;
      this.anchors.push({ el: b, at: new THREE.Vector3(max.x + dx + GAP_LEFT * f + size / 2, top - size / 2 + dy, 0), size, kind: 'icon' });
      this.layer.append(b);
    });
    if (!many) return;
    const n = document.createElement('div');
    n.className = `plate-number ${p.index === s.plateIndex ? 'selected' : ''}`;
    n.textContent = String(p.index);
    this.anchors.push({ el: n, at: new THREE.Vector3(max.x + dx + GAP_LEFT * f + size / 2, min.y + dy + size / 2, 0), size, kind: 'number' });
    this.layer.append(n);
    if (editable) {
      const nameRow = document.createElement('button');
      nameRow.type = 'button';
      nameRow.className = 'plate-name';
      nameRow.title = 'Edit current plate name';
      nameRow.disabled = !idle;
      const text = document.createElement('span');
      text.textContent = p.name;
      const img = document.createElement('img');
      img.src = icon('plate_name_edit');
      img.alt = '';
      nameRow.append(text, img);
      nameRow.onclick = (e) => { e.stopPropagation(); onPlate(() => void editPlateName())(); };
      this.anchors.push({ el: nameRow, at: new THREE.Vector3(min.x + dx, max.y + dy, 0), size, kind: 'name' });
      this.layer.append(nameRow);
    }
  }

  place(camera: THREE.Camera, width: number, height: number, gridlines: boolean) {
    for (const b of this.beds) showBedFor(b, camera, gridlines);
    const p = new THREE.Vector3(), q = new THREE.Vector3();
    const toPx = (v: THREE.Vector3) => [((v.x + 1) / 2) * width, ((1 - v.y) / 2) * height];
    for (const a of this.anchors) {
      p.copy(a.at).project(camera);
      q.copy(a.at).add(new THREE.Vector3(a.size, 0, 0)).project(camera);
      const [x, y] = toPx(p), [x2, y2] = toPx(q);
      const px = Math.min(32, Math.max(12, Math.hypot(x2 - x, y2 - y)));
      a.el.hidden = p.z > 1 || x < -40 || y < -40 || x > width + 40 || y > height + 40;
      if (a.kind === 'name') {
        a.el.style.transform = `translate(${x}px, ${y}px) translate(0, -100%)`;
        a.el.style.setProperty('--plate-icon', `${px}px`);
      } else {
        const cx = a.kind === 'number' || a.kind === 'icon' ? Math.min(x, width - px / 2 - 8) : x;
        a.el.style.transform = `translate(${cx - px / 2}px, ${y - px / 2}px)`;
        a.el.style.width = a.el.style.height = `${px}px`;
        if (a.kind === 'number') a.el.style.fontSize = `${Math.round(px * 0.6)}px`;
      }
    }
  }

  plateAt(s: State, point: THREE.Vector3 | null): number | null {
    if (!point || !this.area.length) return null;
    const { min, max } = bedBounds(this.area);
    for (const p of listedPlates(s)) {
      const [dx, dy] = plateOffset(s, this.area, p.index);
      if (point.x >= min.x + dx && point.x <= max.x + dx && point.y >= min.y + dy && point.y <= max.y + dy) return p.index;
    }
    return null;
  }

  box(s: State): THREE.Box3 {
    const box = new THREE.Box3();
    if (!this.area.length) return box;
    const { min, max } = bedBounds(this.area);
    for (const p of listedPlates(s)) {
      const [dx, dy] = plateOffset(s, this.area, p.index);
      box.expandByPoint(new THREE.Vector3(min.x + dx, min.y + dy, 0)).expandByPoint(new THREE.Vector3(max.x + dx, max.y + dy, 0));
    }
    return box;
  }
}
