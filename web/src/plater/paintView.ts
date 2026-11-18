import * as THREE from 'three';
import { instanceMatrix, volumeWorld } from './geometry';
import { gapFill, isPaint, overhangFacets, overhangGeometry, overlayGeometry, paintOf, paintTool, type Paintable, type Painting } from './paint';
import type { PlateObject, State } from '../state/store';
import { filamentColor } from './filamentColor';
import { slotFilaments } from '../profiles/slots';

const LIGHT_RED = 0xc70000;

export class PaintView {
  readonly group = new THREE.Group();
  private readonly meshes = new Map<string, THREE.Mesh>();
  private readonly material = new THREE.MeshBasicMaterial({ vertexColors: true, polygonOffset: true, polygonOffsetFactor: -1, polygonOffsetUnits: -4 });
  private readonly overhang = new THREE.MeshBasicMaterial({ color: LIGHT_RED, transparent: true, opacity: 0.5, depthWrite: false, polygonOffset: true, polygonOffsetFactor: -1, polygonOffsetUnits: -2 });
  private readonly cursor = new THREE.Mesh(new THREE.RingGeometry(0.92, 1, 48), new THREE.MeshBasicMaterial({ color: 0xa98bd4, side: THREE.DoubleSide, depthTest: false, transparent: true }));
  private readonly overhangCache = new Map<string, THREE.BufferGeometry>();
  private readonly gapCache = new WeakMap<Painting, Map<number, Painting | null>>();

  constructor() {
    this.cursor.visible = false;
    this.cursor.renderOrder = 10;
    this.group.add(this.cursor);
  }

  private put(key: string, geometry: THREE.BufferGeometry, material: THREE.Material, matrix: THREE.Matrix4, seen: Set<string>) {
    seen.add(key);
    let mesh = this.meshes.get(key);
    if (!mesh) {
      mesh = new THREE.Mesh(geometry, material);
      mesh.matrixAutoUpdate = false;
      this.meshes.set(key, mesh);
      this.group.add(mesh);
    }
    mesh.geometry = geometry;
    mesh.matrix.copy(matrix);
    mesh.matrixWorldNeedsUpdate = true;
  }

  private gapPreview(target: Paintable, paint: Painting | null, scale: number): Painting | null {
    const t = paintTool();
    if (t.tool !== 'gap' || !(t.gapArea > 0) || !paint) return paint;
    let byArea = this.gapCache.get(paint);
    if (!byArea) this.gapCache.set(paint, (byArea = new Map()));
    if (!byArea.has(t.gapArea)) byArea.set(t.gapArea, gapFill(target.geometry, paint, t.gapArea / (scale * scale)));
    return byArea.get(t.gapArea)!;
  }

  sync(s: State, preview?: { volume?: number; paint: Painting | null }) {
    const kind = isPaint(s.gizmo) ? s.gizmo : null;
    const o = kind ? s.objects.find((ob) => ob.id === s.selected?.object) : undefined;
    const seen = new Set<string>();
    const palette = [null, ...slotFilaments(s).map((_, i) => new THREE.Color(filamentColor(s, i + 1)).toArray())];
    for (const ob of s.objects) {
      if (ob === o && kind === 'color') continue;
      const targets: [string, Paintable, number | undefined][] = [['o', ob, undefined], ...(ob.volumes ?? []).filter((v) => v.type === 'part').map((v) => [`v${v.id}`, v, v.id] as [string, Paintable, number])];
      for (const inst of ob.instances)
        for (const [id, target, volume] of targets) {
          const paint = paintOf(target, 'color');
          if (paint) this.put(`${inst.id}:${id}:color`, overlayGeometry(target.geometry, paint, palette),
            this.material, volume === undefined ? instanceMatrix(ob, inst) : volumeWorld(ob, inst, ob.volumes!.find((v) => v.id === volume)!), seen);
        }
    }
    if (o && kind) {
      const targets: [string, Paintable, number | undefined][] = [['o', o, undefined], ...(o.volumes ?? []).filter((v) => v.type === 'part').map((v) => [`v${v.id}`, v, v.id] as [string, Paintable, number])];
      for (const inst of o.instances)
        for (const [id, target, volume] of targets) {
          const matrix = volume === undefined ? instanceMatrix(o, inst) : volumeWorld(o, inst, o.volumes!.find((v) => v.id === volume)!);
          const own = preview && preview.volume === volume ? preview.paint : paintOf(target, kind);
          const paint = this.gapPreview(target, own, matrix.getMaxScaleOnAxis());
          if (paint) this.put(`${inst.id}:${id}:paint`, overlayGeometry(target.geometry, paint, kind === 'color' ? palette : undefined), this.material, matrix, seen);
          const deg = kind === 'support' ? paintTool().highlight : 0;
          const normal = new THREE.Matrix3().getNormalMatrix(matrix);
          const key = `${target.geometry.uuid}:${deg}:${normal.elements.map((e) => e.toFixed(4)).join()}`;
          const allow = overhangFacets(target.geometry, normal, deg);
          if (allow) {
            if (!this.overhangCache.has(key)) this.overhangCache.set(key, overhangGeometry(target.geometry, allow));
            this.put(`${inst.id}:${id}:overhang`, this.overhangCache.get(key)!, this.overhang, matrix, seen);
          }
        }
    }
    for (const [key, mesh] of this.meshes)
      if (!seen.has(key)) { this.group.remove(mesh); this.meshes.delete(key); }
  }

  showCursor(point: THREE.Vector3 | null, normal?: THREE.Vector3, radius = 1) {
    this.cursor.visible = !!point && radius > 0;
    if (!point || !normal) return;
    this.cursor.position.copy(point).addScaledVector(normal, 0.05);
    this.cursor.quaternion.setFromUnitVectors(new THREE.Vector3(0, 0, 1), normal);
    this.cursor.scale.setScalar(radius);
    this.cursor.updateMatrix();
  }
}

export const meshScale = (o: PlateObject, inst: PlateObject['instances'][number]) => instanceMatrix(o, inst).getMaxScaleOnAxis();
