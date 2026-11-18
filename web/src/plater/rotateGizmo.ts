import * as THREE from 'three';

export type RotateAxis = 0 | 1 | 2;
const AXES = [new THREE.Vector3(1, 0, 0), new THREE.Vector3(0, 1, 0), new THREE.Vector3(0, 0, 1)];
const COLORS = [0xe5484d, 0x46a758, 0x3e7ee8];
const HOVER = [0xff8a8d, 0x86d993, 0x8bb4ff];
const LINE_PX = 2, HOVER_PX = 3.5, PICK_PX = 9, PICK_TOUCH_PX = 20;
const OFFSET_MM = 5;
const SCALE_STEPS = 72, LONG_TOOTH = 0.1, SHORT_TOOTH = 0.05;

function basis(axis: RotateAxis): [THREE.Vector3, THREE.Vector3] {
  return axis === 0 ? [AXES[1], AXES[2]] : axis === 1 ? [AXES[2], AXES[0]] : [AXES[0], AXES[1]];
}

export class RotateGizmo {
  readonly group = new THREE.Group();
  private readonly rings: THREE.Mesh[] = [];
  private readonly ticks = new THREE.LineSegments(new THREE.BufferGeometry(), new THREE.LineBasicMaterial({ depthTest: false, transparent: true, opacity: 0.9 }));
  private readonly arc = new THREE.Line(new THREE.BufferGeometry(), new THREE.LineBasicMaterial({ depthTest: false, transparent: true }));
  private readonly readout: HTMLDivElement;
  private readonly center = new THREE.Vector3();
  private radius = 10;
  private hover: RotateAxis | null = null;
  private drag: { axis: RotateAxis; start: number; angle: number; pointer: number; tangent?: { x: number; y: number; dx: number; dy: number; perRad: number } } | null = null;
  private readonly ray = new THREE.Raycaster();
  private readonly plane = new THREE.Plane();
  private attached = false;

  constructor(private readonly dom: HTMLElement, private readonly camera: () => THREE.Camera, private readonly changed: () => void,
    private readonly events: { start(axis: RotateAxis): void; change(axis: RotateAxis, angle: number): void; end(): void }) {
    for (let a = 0; a < 3; a++) {
      const ring = new THREE.Mesh(new THREE.TorusGeometry(1, 0.01, 6, 128), new THREE.MeshBasicMaterial({ color: COLORS[a], depthTest: false, transparent: true }));
      ring.renderOrder = 20;
      if (a === 0) ring.rotation.y = Math.PI / 2;
      if (a === 1) ring.rotation.x = Math.PI / 2;
      ring.raycast = () => {};
      this.rings.push(ring);
      this.group.add(ring);
    }
    this.ticks.renderOrder = this.arc.renderOrder = 21;
    this.group.add(this.ticks, this.arc);
    this.group.visible = false;
    this.readout = document.createElement('div');
    this.readout.className = 'gizmo-readout';
    this.readout.hidden = true;
    dom.parentElement?.appendChild(this.readout);
    dom.addEventListener('pointerdown', this.onDown, { capture: true });
    dom.addEventListener('pointermove', this.onMove, { capture: true });
    dom.addEventListener('pointerup', this.onUp, { capture: true });
    dom.addEventListener('pointercancel', this.onUp, { capture: true });
  }

  get dragging() { return !!this.drag; }

  attach(center: THREE.Vector3, sphereRadius: number) {
    if (this.drag) return;
    this.center.copy(center);
    this.radius = sphereRadius + OFFSET_MM;
    this.attached = true;
    this.group.visible = true;
  }

  detach() {
    if (this.drag) this.finish();
    this.attached = false;
    this.group.visible = false;
    this.setHover(null);
  }

  private perPixel(): number {
    const cam = this.camera(), h = this.dom.clientHeight || 1;
    if (cam instanceof THREE.OrthographicCamera) return (cam.top - cam.bottom) / cam.zoom / h;
    const p = cam as THREE.PerspectiveCamera;
    return 2 * p.position.distanceTo(this.center) * Math.tan(THREE.MathUtils.degToRad(p.fov) / 2) / h;
  }

  update() {
    if (!this.attached) return;
    const px = this.perPixel(), r = this.radius;
    this.group.position.copy(this.center);
    this.rings.forEach((ring, a) => {
      ring.visible = this.drag ? this.drag.axis === a : true;
      const tube = Math.max((this.hover === a || this.drag?.axis === a ? HOVER_PX : LINE_PX) * px / r / 2, 1e-4);
      const g = ring.geometry as THREE.TorusGeometry;
      if (Math.abs(g.parameters.tube - tube) > tube * 0.02) {
        g.dispose();
        ring.geometry = new THREE.TorusGeometry(1, tube, 6, 128);
      }
      ring.scale.setScalar(r);
    });
  }

  private setHover(axis: RotateAxis | null) {
    if (this.hover === axis) return;
    this.hover = axis;
    this.rings.forEach((ring, a) => (ring.material as THREE.MeshBasicMaterial).color.setHex(axis === a ? HOVER[a] : COLORS[a]));
    this.dom.style.cursor = axis === null ? '' : 'grab';
    this.changed();
  }

  private rayAt(ev: PointerEvent) {
    const r = this.dom.getBoundingClientRect();
    this.ray.setFromCamera(new THREE.Vector2((ev.clientX - r.left) / r.width * 2 - 1, -(ev.clientY - r.top) / r.height * 2 + 1), this.camera());
    return this.ray.ray;
  }

  private inPlane(ev: PointerEvent, axis: RotateAxis): THREE.Vector3 | null {
    const ray = this.rayAt(ev);
    this.plane.setFromNormalAndCoplanarPoint(AXES[axis], this.center);
    if (Math.abs(ray.direction.dot(AXES[axis])) < 1e-3) return null;
    const hit = ray.intersectPlane(this.plane, new THREE.Vector3());
    return hit && hit.sub(this.center);
  }

  private pick(ev: Pick<PointerEvent, 'clientX' | 'clientY' | 'pointerType'>): { axis: RotateAxis; t: number } | null {
    const rect = this.dom.getBoundingClientRect(), cam = this.camera(), x = ev.clientX - rect.left, y = ev.clientY - rect.top;
    const tol = ev.pointerType === 'touch' ? PICK_TOUCH_PX : PICK_PX;
    let best: { axis: RotateAxis; t: number } | null = null, bestD = tol, bestZ = Infinity;
    const p = new THREE.Vector3();
    for (let a = 0 as RotateAxis; a < 3; a = (a + 1) as RotateAxis) {
      const [u, v] = basis(a);
      for (let i = 0; i < 96; i++) {
        const t = i / 96 * Math.PI * 2;
        p.copy(this.center).addScaledVector(u, Math.cos(t) * this.radius).addScaledVector(v, Math.sin(t) * this.radius).project(cam);
        if (p.z > 1) continue;
        const d = Math.hypot((p.x + 1) / 2 * rect.width - x, (1 - p.y) / 2 * rect.height - y);
        if (d < bestD - 1 || (d < bestD + 1 && d <= tol && p.z < bestZ)) { bestD = Math.min(d, bestD); bestZ = p.z; best = { axis: a, t }; }
      }
    }
    return best;
  }

  private tangentAt(axis: RotateAxis, t: number, ev: PointerEvent) {
    const [u, v] = basis(axis), rect = this.dom.getBoundingClientRect(), cam = this.camera(), eps = 0.05;
    const at = (a: number) => {
      const p = this.center.clone().addScaledVector(u, Math.cos(a) * this.radius).addScaledVector(v, Math.sin(a) * this.radius).project(cam);
      return [(p.x + 1) / 2 * rect.width, (1 - p.y) / 2 * rect.height];
    };
    const [x0, y0] = at(t - eps), [x1, y1] = at(t + eps), len = Math.hypot(x1 - x0, y1 - y0) || 1;
    return { x: ev.clientX, y: ev.clientY, dx: (x1 - x0) / len, dy: (y1 - y0) / len, perRad: Math.max(this.radius / this.perPixel(), 20) };
  }

  private angleOf(q: THREE.Vector3, axis: RotateAxis) {
    const [u, v] = basis(axis);
    return Math.atan2(q.dot(v), q.dot(u));
  }

  private onDown = (ev: PointerEvent) => {
    if (!this.attached || ev.button !== 0 || this.drag) return;
    const hit = this.pick(ev);
    if (!hit) return;
    const { axis } = hit;
    ev.stopImmediatePropagation(); ev.preventDefault();
    this.dom.setPointerCapture(ev.pointerId);
    const q = Math.abs(this.rayAt(ev).direction.dot(AXES[axis])) > 0.25 ? this.inPlane(ev, axis) : null;
    this.drag = { axis, start: q ? this.angleOf(q, axis) : hit.t, angle: 0, pointer: ev.pointerId, tangent: q ? undefined : this.tangentAt(axis, hit.t, ev) };
    this.dom.style.cursor = 'grabbing';
    this.events.start(axis);
    this.drawScale(axis);
    this.changed();
  };

  private onMove = (ev: PointerEvent) => {
    if (!this.attached) return;
    if (!this.drag) { if (!ev.buttons) this.setHover(this.pick(ev)?.axis ?? null); return; }
    if (ev.pointerId !== this.drag.pointer) return;
    ev.stopImmediatePropagation();
    const { axis, tangent } = this.drag, q = this.inPlane(ev, axis);
    let angle: number;
    if (tangent) angle = ((ev.clientX - tangent.x) * tangent.dx + (ev.clientY - tangent.y) * tangent.dy) / tangent.perRad;
    else {
      if (!q) return;
      angle = this.angleOf(q, axis) - this.drag.start;
      angle = Math.atan2(Math.sin(angle), Math.cos(angle));
    }
    const last = this.drag.angle;
    while (angle - last > Math.PI) angle -= 2 * Math.PI;
    while (angle - last < -Math.PI) angle += 2 * Math.PI;
    const len = q && !tangent ? q.length() / this.radius : 0;
    const step = ev.shiftKey ? Math.PI / 12 : len >= 1 / 3 && len <= 2 / 3 ? Math.PI / 4 : len >= 1 && len <= 1 + LONG_TOOTH ? 2 * Math.PI / SCALE_STEPS : 0;
    if (step) angle = step * Math.round(angle / step);
    if (angle === this.drag.angle) return;
    this.drag.angle = angle;
    this.drawArc(axis, this.drag.start, angle);
    this.readout.hidden = false;
    this.readout.textContent = `${'XYZ'[axis]} ${(angle * 180 / Math.PI).toFixed(1).replace(/\.0$/, '')}°`;
    const host = this.readout.parentElement!.getBoundingClientRect();
    this.readout.style.left = `${ev.clientX - host.left + 14}px`;
    this.readout.style.top = `${ev.clientY - host.top + 14}px`;
    this.events.change(axis, angle);
    this.changed();
  };

  private onUp = (ev: PointerEvent) => {
    if (!this.drag || ev.pointerId !== this.drag.pointer) return;
    ev.stopImmediatePropagation();
    if (this.dom.hasPointerCapture(ev.pointerId)) this.dom.releasePointerCapture(ev.pointerId);
    this.finish();
  };

  cancel() {
    if (!this.drag) return;
    if (this.dom.hasPointerCapture(this.drag.pointer)) this.dom.releasePointerCapture(this.drag.pointer);
    this.finish();
  }

  private finish() {
    this.drag = null;
    this.readout.hidden = true;
    this.ticks.visible = this.arc.visible = false;
    this.dom.style.cursor = this.hover === null ? '' : 'grab';
    this.events.end();
    this.changed();
  }

  private drawScale(axis: RotateAxis) {
    const [u, v] = basis(axis), pts: number[] = [], r = this.radius;
    for (let i = 0; i < SCALE_STEPS; i++) {
      const t = i / SCALE_STEPS * Math.PI * 2, c = Math.cos(t), s = Math.sin(t), out = r * (1 + (i % 2 === 0 ? LONG_TOOTH : SHORT_TOOTH));
      const a = u.clone().multiplyScalar(c * r).addScaledVector(v, s * r), b = u.clone().multiplyScalar(c * out).addScaledVector(v, s * out);
      pts.push(a.x, a.y, a.z, b.x, b.y, b.z);
    }
    this.ticks.geometry.dispose();
    this.ticks.geometry = new THREE.BufferGeometry().setAttribute('position', new THREE.Float32BufferAttribute(pts, 3));
    (this.ticks.material as THREE.LineBasicMaterial).color.setHex(COLORS[axis]);
    this.ticks.visible = true;
  }

  private drawArc(axis: RotateAxis, start: number, angle: number) {
    const [u, v] = basis(axis), pts: number[] = [], r = this.radius * 0.92, n = Math.max(2, Math.ceil(Math.abs(angle) / (Math.PI / 64)));
    pts.push(0, 0, 0);
    for (let i = 0; i <= n; i++) {
      const t = start + angle * i / n;
      const p = u.clone().multiplyScalar(Math.cos(t) * r).addScaledVector(v, Math.sin(t) * r);
      pts.push(p.x, p.y, p.z);
    }
    pts.push(0, 0, 0);
    this.arc.geometry.dispose();
    this.arc.geometry = new THREE.BufferGeometry().setAttribute('position', new THREE.Float32BufferAttribute(pts, 3));
    (this.arc.material as THREE.LineBasicMaterial).color.setHex(HOVER[axis]);
    this.arc.visible = true;
  }

  dispose() {
    this.dom.removeEventListener('pointerdown', this.onDown, { capture: true });
    this.dom.removeEventListener('pointermove', this.onMove, { capture: true });
    this.dom.removeEventListener('pointerup', this.onUp, { capture: true });
    this.dom.removeEventListener('pointercancel', this.onUp, { capture: true });
    this.readout.remove();
    for (const r of this.rings) { r.geometry.dispose(); (r.material as THREE.Material).dispose(); }
    this.ticks.geometry.dispose(); this.arc.geometry.dispose();
  }
}
