import { tools } from '../flow/tools';
import { takeProbedCanvas } from '../browser-support';
import { useEffect, useRef } from 'react';
import * as THREE from 'three';
import { CameraViews } from './CameraViews';
import { TransformControls } from 'three/examples/jsm/controls/TransformControls.js';
import { effectiveConfig, getState, isSelected, markStale, selectionOf, toggleSelection, printerPreset, setPaint, setState, subscribeStore, updateInstance, updateObject, updateVolume, useStore, type CutConnector, type Instance, type PlateObject, type SeamMode, type State, type Volume, type VolumeType } from '../state/store';
import { HOVER_COLOR, SELECTION_COLOR, SelectionBounds, selectedSurface } from './selectionVisual';
import { PaintView } from './paintView';
import { viewOptions, subscribeViewOptions } from './viewOptions';
import { sectionView, subscribeSection } from './section';
import { defaultEarRadius, earsUi, setEars, setEarsUi, subscribeEarsUi } from './brimEars';
import { brush, bucketFill, fill as fillPaint, heightRange, HEIGHT, isPaint, overhangFacets, overhangGeometry, paintOf, paintTool, paintTriangle, PAINT_STEP, RADIUS, setPaintTool, subscribePaintTool, type PaintKind, type Painting } from './paint';
import { beginStep, undoable } from '../state/history';
import { TowerView } from './wipeTower';
import { setTextPlacing, textPlacing } from './textPlacing';
import { assembly, subscribeAssembly } from './assembly';
import { gridOrigin, plateOffset, PlateGrid } from './plateGrid';
import { canAddPlate, moveToPlate, selectPlate } from './plates';
import { filamentColor } from './filamentColor';
import { hoverLink, setHoverLink, subscribeHoverLink } from './hoverLink';
import { bedKey as bedCacheKey, buildBed, sceneColors, showBedFor } from './Bed';
import { subscribeTheme } from '../theme';
import { pixelRatio, watchPixelRatio } from './pixelRatio';
import { raycastBvh, type Bvh } from './bvh';
import { drawn, instanceBox, instanceMatrix, outsideBed, parsePoints, requestDisplay, volumeWorld } from './geometry';
import { cutNormal, cutPoint, cutTarget, setCut } from './cut';
import { setSlant, slantAngles, slantNormal, slantPoint, slantTarget } from './slant';
import { CONNECTOR, connectorAt, connectorWorld, planeRotation, planeUV, SECTORS } from './cutJoints';
import { layOnFace, layOnFacePlanes, type LayOnFacePlane } from './transforms';
import { RotateGizmo } from './rotateGizmo';
import { MeasureView } from './measureView';
import { measureState, setMeasure, subscribeMeasure } from './measureState';
import { layerCursor, layerParams, layerProfileOf, subscribeLayerCursor } from '../components/LayerHeight';
import { layerMaterial } from './layerTexture';
import { getPatchPreview, inFootprint, nonplanarViewShown, overlayFits, planarReason, subscribePatchPreview, type InstanceOverlay } from '../patch-preview';
import { hoverSurface, selectSurface, subscribeTopSurfaces, surfaceAt, surfaceText, topSurfaces, topSurfacesShown } from '../top-surfaces';

const OUTSIDE = new THREE.Color(0x4d9ed9);
const WHITE = new THREE.Color(0xffffff);
const MIN_LIGHTNESS = 0.28;
const VOLUME_COLOURS: Record<Exclude<VolumeType, 'part'>, [number, number, number, number]> = {
  modifier: [1, 1, 0, 0.6], negative: [0.3, 0.3, 0.3, 0.4], support_enforcer: [0.3, 0.3, 1, 0.4], support_blocker: [1, 0.3, 0.3, 0.4],
  seam: [1, 0.627, 0.082, 0.6],
};
const SEAM_COLOURS: Record<SeamMode, [number, number, number, number]> = {
  center: [1, 0.627, 0.082, 0.6], left: [1, 0.753, 0, 0.6], right: [1, 0.514, 0, 0.6], enforced: [0.412, 0.82, 0.412, 0.6],
  blocked: [0.82, 0.412, 0.412, 0.6], neutral: [0.655, 0.655, 0.655, 0.6],
};
const CUT_PLANE = new THREE.MeshBasicMaterial({ color: new THREE.Color(0.9, 0.9, 0.9), transparent: true, opacity: 0.5, side: THREE.DoubleSide, depthWrite: false });

function brightenColor(c: THREE.Color): THREE.Color {
  const hsl = c.getHSL({ h: 0, s: 0, l: 0 }, THREE.SRGBColorSpace);
  return c.clone().setHSL(hsl.h, hsl.s, Math.min(hsl.l + 0.25, 1), THREE.SRGBColorSpace);
}

export default function Plater3D() {
  const host = useRef<HTMLDivElement>(null);
  const gizmo = useStore((s) => s.gizmo);
  const gizmoRef = useRef<(g: State['gizmo']) => void>(() => {});

  useEffect(() => {
    const el = host.current!;
    const renderer = new THREE.WebGLRenderer({ canvas: takeProbedCanvas(), antialias: true, preserveDrawingBuffer: true });
    renderer.setClearColor(sceneColors().background);
    renderer.localClippingEnabled = true;
    el.appendChild(renderer.domElement);

    const scene = new THREE.Scene();
    scene.add(new THREE.HemisphereLight(0xffffff, 0x737373, 1.15));
    const key = new THREE.DirectionalLight(0xffffff, 2.1);
    const fill = new THREE.DirectionalLight(0xffffff, 0.8);
    scene.add(key, fill);
    const keyFrom = new THREE.Vector3(-0.55, 0.6, 0.6).normalize(), fillFrom = new THREE.Vector3(0.65, -0.35, 0.45).normalize();
    const lights = (camera: THREE.Camera) => {
      key.position.copy(keyFrom).applyQuaternion(camera.quaternion);
      fill.position.copy(fillFrom).applyQuaternion(camera.quaternion);
    };

    const followers: { tc?: TransformControls } = {};
    const views = new CameraViews(renderer.domElement, () => getState().tab === 'prepare', () => {
      if (followers.tc) followers.tc.camera = views.camera;
      render();
    });
    const controls = views.controls;
    let frame = 0;
    let moving = false;
    let settle: ReturnType<typeof setTimeout> | undefined;
    controls.addEventListener('start', () => { clearTimeout(settle); moving = true; });
    controls.addEventListener('end', () => {
      clearTimeout(settle);
      settle = setTimeout(() => { moving = false; render(); }, 180);
    });
    const frames: { at: number; ms: number }[] = [];
    let rotateGizmo: RotateGizmo | null = null;
    let slantRingsRef: RotateGizmo | null = null;
    const render = () => {
      if (frame || !el.clientWidth || getState().tab !== 'prepare') return;
      frame = requestAnimationFrame(() => {
        frame = 0;
        if (!el.clientWidth || getState().tab !== 'prepare') return;
        const t0 = performance.now();
        if (uploads.length && !moving) uploadStep();
        const motion = moving && [...meshes.values(), ...volumeMeshes.values()].some((m) => m.geometry.userData.motionLod) && !isPaint(getState().gizmo);
        const ratio = pixelRatio(el.clientWidth, el.clientHeight) * (motion ? 0.5 : 1);
        if (renderer.getPixelRatio() !== ratio) renderer.setPixelRatio(ratio);
        showBedFor(bed, views.camera, viewOptions().gridlines);
        if (bed) bed.visible = !assembly().on;
        grid.group.visible = !assembly().on;
        grid.layer.hidden = assembly().on;
        views.ensureFinite();
        rotateGizmo?.update();
        slantRingsRef?.update();
        renderer.render(scene, views.camera);
        placeLabels();
        grid.place(views.camera, el.clientWidth, el.clientHeight, viewOptions().gridlines);
        if (flattenScene.children.length) {
          renderer.autoClear = false;
          renderer.clearDepth();
          renderer.render(flattenScene, views.camera);
          renderer.autoClear = true;
        }
        if (measuring.scene.children.length || getState().gizmo === 'measure') {
          measuring.frame();
          renderer.autoClear = false;
          renderer.render(measuring.scene, views.camera);
          renderer.autoClear = true;
        }
        if (frames.length > 4096) frames.splice(0, 2048);
        frames.push({ at: t0, ms: performance.now() - t0 });
      });
    };

    const STAGED_BYTES = 8 << 20;
    const uploads: { geo: THREE.BufferGeometry; steps: (THREE.BufferAttribute | THREE.InterleavedBufferAttribute)[] }[] = [];
    const uploaded = new WeakSet<THREE.BufferGeometry>();
    const stageGeo = new THREE.BufferGeometry();
    stageGeo.setDrawRange(0, 0);
    stageGeo.boundingSphere = new THREE.Sphere();
    const dummy = new THREE.BufferAttribute(new Float32Array(3), 3);
    stageGeo.setAttribute('position', dummy);
    const stage = new THREE.Mesh(stageGeo, new THREE.MeshBasicMaterial());
    stage.frustumCulled = false;
    const stageScene = new THREE.Scene().add(stage);
    void renderer.compileAsync(stageScene, views.camera);
    const needsUpload = (geo: THREE.BufferGeometry) => {
      if (drawn(geo) || uploaded.has(geo)) return false;
      const steps = [...Object.values(geo.attributes), ...(geo.index ? [geo.index] : [])];
      if (steps.reduce((n, a) => n + a.array.byteLength, 0) < STAGED_BYTES) return false;
      if (!uploads.some((u) => u.geo === geo)) uploads.push({ geo, steps });
      return true;
    };
    const uploadStep = () => {
      const u = uploads[0];
      const a = u.steps.shift()!;
      for (const k of Object.keys(stageGeo.attributes)) stageGeo.deleteAttribute(k);
      stageGeo.setIndex(null);
      stageGeo.setAttribute('position', a === u.geo.index ? dummy : (a as THREE.BufferAttribute));
      if (a === u.geo.index) stageGeo.setIndex(a as THREE.BufferAttribute);
      renderer.render(stageScene, views.camera);
      stageGeo.setAttribute('position', dummy);
      stageGeo.setIndex(null);
      if (!u.steps.length) {
        uploads.shift();
        uploaded.add(u.geo);
        for (const m of meshes.values()) if (m.geometry === u.geo) m.visible = true;
        tint(getState());
      }
      render();
    };
    controls.addEventListener('change', () => {
      clearTimeout(settle); moving = true;
      settle = setTimeout(() => { moving = false; render(); }, 180);
      render();
    });

    let bed: THREE.Group | null = null;
    let bedKey = '';
    let area: THREE.Vector2[] = [];
    let height = 250;
    const fitCamera = () => views.setBed(area);
    const grid = new PlateGrid((color) => material(color, 'normal'));
    scene.add(grid.group);
    let gridAt: { index: number; count: number } | null = null;
    const syncGrid = (s: State) => {
      const count = s.projectPlates.length;
      if (gridAt && area.length && (gridAt.index !== s.plateIndex || gridAt.count !== count)) {
        const a = gridOrigin(gridAt.index, gridAt.count, area), b = gridOrigin(s.plateIndex, count, area);
        views.shift(a[0] - b[0], a[1] - b[1]);
      }
      const grew = gridAt !== null && count > gridAt.count;
      gridAt = { index: s.plateIndex, count };
      grid.sync(s, bed, area);
      if (grew) views.fit(grid.box(s));
    };
    const viewExtras = new THREE.Group();
    const COM_BALL = new THREE.SphereGeometry(2.5, 32, 16);
    {
      const pos = COM_BALL.attributes.position, colors: number[] = [];
      for (let i = 0; i < pos.count; i++) { const k = pos.getX(i) * pos.getY(i) * pos.getZ(i) > 0 ? 0.05 : 0.95; colors.push(k, k, k); }
      COM_BALL.setAttribute('color', new THREE.Float32BufferAttribute(colors, 3));
    }
    const COM_MATERIAL = new THREE.MeshBasicMaterial({ vertexColors: true, depthTest: false });
    const centroids = new WeakMap<THREE.BufferGeometry, THREE.Vector3>();
    const centroidOf = (g: THREE.BufferGeometry) => {
      let c = centroids.get(g);
      if (c) return c;
      const p = g.attributes.position, idx = g.index, n = idx ? idx.count : p.count;
      const a = new THREE.Vector3(), b = new THREE.Vector3(), d = new THREE.Vector3(), sum = new THREE.Vector3();
      let vol = 0;
      for (let t = 0; t < n; t += 3) {
        a.fromBufferAttribute(p, idx ? idx.getX(t) : t); b.fromBufferAttribute(p, idx ? idx.getX(t + 1) : t + 1); d.fromBufferAttribute(p, idx ? idx.getX(t + 2) : t + 2);
        const v = a.dot(b.clone().cross(d)) / 6;
        vol += v;
        sum.addScaledVector(a.clone().add(b).add(d), v / 4);
      }
      c = vol ? sum.divideScalar(vol) : (g.computeBoundingBox(), g.boundingBox!.getCenter(new THREE.Vector3()));
      centroids.set(g, c);
      return c;
    };
    scene.add(viewExtras);
    const surfaceTint = new THREE.MeshBasicMaterial({ vertexColors: true, polygonOffset: true, polygonOffsetFactor: -2, polygonOffsetUnits: -4 });
    const overhangTint = new THREE.MeshBasicMaterial({ color: 0xc70000, transparent: true, opacity: 0.5, depthWrite: false, polygonOffset: true, polygonOffsetFactor: -1, polygonOffsetUnits: -2 });
    const outlineLine = new THREE.LineBasicMaterial({ color: SELECTION_COLOR });
    const wireLine = new THREE.LineBasicMaterial({ color: 0x1a1a1a, transparent: true, opacity: 0.45 });
    const extrasCache = new Map<string, THREE.BufferGeometry>();
    const outlineCache = new Map<THREE.BufferGeometry, THREE.BufferGeometry>();
    const labelLayer = document.createElement('div');
    labelLayer.className = 'canvas-labels';
    const syncView = (s: State) => {
      viewExtras.clear();
      const opt = viewOptions(), seen = new Set<string>();
      for (const o of s.objects) {
        if (o.project?.placeholder) continue;
        for (const inst of o.instances) {
          const m = instanceMatrix(o, inst);
          const put = (key: string, make: () => THREE.BufferGeometry, mesh: (g: THREE.BufferGeometry) => THREE.Object3D) => {
            seen.add(key);
            let g = extrasCache.get(key);
            if (!g) extrasCache.set(key, (g = make()));
            const obj = mesh(g);
            obj.matrixAutoUpdate = false;
            obj.matrix.copy(m);
            viewExtras.add(obj);
          };
          if (opt.overhang) {
            const deg = Number({ ...effectiveConfig(s, 'process'), ...o.settings }.support_threshold_angle) || 30;
            const normal = new THREE.Matrix3().getNormalMatrix(m);
            put(`oh:${o.geometry.uuid}:${deg}:${normal.elements.map((e) => e.toFixed(4)).join()}`,
              () => overhangGeometry(o.geometry, overhangFacets(o.geometry, normal, deg)!), (g) => new THREE.Mesh(g, overhangTint));
          }
          if (opt.wireframe) put(`wf:${o.geometry.uuid}`, () => new THREE.WireframeGeometry(o.geometry), (g) => new THREE.LineSegments(g, wireLine));
          if (opt.outline && selectedSurface(s.selected, o.id, inst.id)) {
            const display = drawn(o.geometry)?.display ?? o.geometry;
            if ((display.index?.count ?? display.attributes.position.count) <= 300_000)
              put(`ol:${display.uuid}`, () => {
                let edges = outlineCache.get(display);
                if (!edges) outlineCache.set(display, (edges = new THREE.EdgesGeometry(display, 30)));
                return edges;
              }, (g) => new THREE.LineSegments(g, outlineLine));
          }
        }
      }
      for (const [k, g] of extrasCache) if (!seen.has(k)) { if (!k.startsWith('ol:')) g.dispose(); extrasCache.delete(k); }
      const sources = new Set(s.objects.flatMap(o => [o.geometry, drawn(o.geometry)?.display]));
      for (const [source, edges] of outlineCache) if (!sources.has(source)) { edges.dispose(); outlineCache.delete(source); }
      if (opt.axes && area.length) {
        const xs = area.map((p) => p.x), ys = area.map((p) => p.y);
        const len = 0.1 * Math.max(Math.max(...xs) - Math.min(...xs), Math.max(...ys) - Math.min(...ys), height);
        [[1, 0, 0, 0xe62e2e], [0, 1, 0, 0x2ee62e], [0, 0, 1, 0x2e5ce6]].forEach(([x, y, z, c]) =>
          viewExtras.add(new THREE.ArrowHelper(new THREE.Vector3(x, y, z), new THREE.Vector3(0, 0, 0.01), len, c, len * 0.25, len * 0.12)));
      }
      if (opt.com && ['none', 'move', 'rotate', 'scale', 'flatten'].includes(s.gizmo))
        for (const o of s.objects) {
          if (o.project?.placeholder) continue;
          const c = centroidOf(o.geometry);
          for (const inst of o.instances) {
            const ball = new THREE.Mesh(COM_BALL, COM_MATERIAL);
            ball.position.copy(c).applyMatrix4(instanceMatrix(o, inst));
            ball.renderOrder = 4;
            viewExtras.add(ball);
          }
        }
      if (topSurfacesShown())
        for (const ts of topSurfaces()) {
          const o = s.objects.find((ob) => ob.id === ts.objectId), inst = o?.instances.find((i) => i.id === ts.instanceId);
          if (!o || !inst || ts.source !== o.geometry || !ts.geometry.attributes.position.count) continue;
          const mesh = new THREE.Mesh(ts.geometry, surfaceTint);
          mesh.matrixAutoUpdate = false;
          mesh.matrix.copy(instanceMatrix(o, inst));
          viewExtras.add(mesh);
        }
      labelLayer.replaceChildren(...(opt.labels ? s.objects.flatMap((o) => o.instances.map((inst, i) => {
        const d = document.createElement('div');
        d.className = 'canvas-label';
        d.textContent = o.instances.length > 1 ? `${o.name} (${i + 1})` : o.name;
        d.dataset.object = String(o.id);
        d.dataset.instance = String(inst.id);
        return d;
      })) : []));
    };
    const placeLabels = () => {
      if (!labelLayer.childElementCount) return;
      const s = getState(), w = el.clientWidth, h = el.clientHeight;
      for (const d of labelLayer.children as HTMLCollectionOf<HTMLElement>) {
        const o = s.objects.find((ob) => ob.id === Number(d.dataset.object)), inst = o?.instances.find((i) => i.id === Number(d.dataset.instance));
        if (!o || !inst) continue;
        const b = instanceBox(o, inst), p = new THREE.Vector3((b.min.x + b.max.x) / 2, (b.min.y + b.max.y) / 2, b.max.z).project(views.camera);
        d.style.transform = `translate(-50%, -100%) translate(${((p.x + 1) / 2) * w}px, ${((1 - p.y) / 2) * h}px)`;
        d.hidden = p.z > 1;
      }
    };

    const syncBed = (s: State) => {
      const m = printerPreset(s);
      const key = bedCacheKey(m);
      if (key === bedKey) return;
      bedKey = key;
      if (bed) scene.remove(bed);
      bed = buildBed(m, render);
      scene.add(bed);
      area = parsePoints(m?.config.printable_area);
      height = Number(m?.config.printable_height ?? 250) || 250;
      fitCamera();
    };

    const meshes = new Map<number, THREE.Mesh>();
    const materials = new Map<string, THREE.MeshStandardMaterial>();
    const material = (color: string, state: 'normal' | 'selected' | 'outside', placeholder = false, ghost = false, xray = false, selected = state === 'selected', hovered = false, nonprintable = false) => {
      const k = color + state + placeholder + ghost + xray + selected + hovered + nonprintable;
      let m = materials.get(k);
      if (!m) {
        let c = new THREE.Color(color);
        const hsl = c.getHSL({ h: 0, s: 0, l: 0 }, THREE.SRGBColorSpace);
        if (hsl.l < MIN_LIGHTNESS) c.setHSL(hsl.h, hsl.s, MIN_LIGHTNESS, THREE.SRGBColorSpace);
        if (state !== 'outside' && !nonprintable && selected) c.lerp(WHITE, 0.2);
        else if (state !== 'outside' && !nonprintable && hovered) c.lerp(WHITE, 0.1);
        if (state === 'outside') c.lerp(OUTSIDE, 0.7);
        m = new THREE.MeshStandardMaterial({ color: c, emissive: selected || hovered ? 0xffffff : 0, emissiveIntensity: selected ? 0.05 : 0.03, roughness: 0.55, metalness: 0.05, transparent: placeholder || ghost || xray, clippingPlanes: sectionView().ratio > 0 ? [sectionPlane] : null,
          polygonOffset: true, polygonOffsetFactor: 1, polygonOffsetUnits: 1,
          opacity: placeholder ? 0.35 : ghost ? 0.4 : xray ? 0.25 : 1, depthWrite: !ghost && !xray, side: xray ? THREE.DoubleSide : THREE.FrontSide });
        materials.set(k, m);
      }
      return m;
    };
    {
      const warm = new THREE.Mesh(new THREE.BoxGeometry(1, 1, 1), material('#00AE42', 'normal'));
      scene.add(warm);
      void renderer.compileAsync(warm, views.camera, scene).finally(() => {
        scene.remove(warm);
        warm.geometry.dispose();
      });
    }
    let liveGeometries = new Set<THREE.BufferGeometry>();
    let outside = new Set<number>();
    let hovered: number | null = null;
    let hoveredVolume: number | undefined;
    const selectionBounds = new SelectionBounds();
    scene.add(selectionBounds.group);
    const selectionColours = () => {
      const hex = (name: string, fallback: number) => { const v = parseInt(getComputedStyle(document.documentElement).getPropertyValue(name).trim().slice(1), 16); return Number.isNaN(v) ? fallback : v; };
      const outline = hex('--bed-outline', SELECTION_COLOR);
      selectionBounds.recolor(outline, hex('--line-strong', HOVER_COLOR));
      outlineLine.color.setHex(outline);
    };
    selectionColours();
    const tint = (s: State) => {
      const xray = viewOptions().xray && !isPaint(s.gizmo) && s.gizmo !== 'layers';
      const link = hoverLink();
      const over = (object: number, instance: number, volume?: number) => link?.source === 'list'
        ? link.object === object && (link.instance === undefined || link.instance === instance)
        : hovered === instance && hoveredVolume === volume;
      selectionBounds.begin();
      const dimmed = (o: number, i: number) => link?.source === 'list' && (o !== link.object || (link.instance !== undefined && i !== link.instance));
      for (const o of s.objects) {
        const color = topSurfacesShown() ? '#A8A8A8' : filamentColor(s, o.extruder);
        for (const inst of o.instances) {
          const mesh = meshes.get(inst.id);
          if (!mesh) continue;
          const selected = selectedSurface(s.selected, o.id, inst.id), hovered = over(o.id, inst.id);
          const state = outside.has(inst.id) ? 'outside' : 'normal';
          const ghost = isSelected(s.selected, o.id, inst.id) && s.selected?.volume !== undefined;
          mesh.material = s.gizmo === 'layers' && s.selected?.object === o.id
            ? layerMaterial(layerParams(s, o), layerProfileOf(s, o), layerCursor())
            : material(inst.printable === false ? '#999999' : color, inst.printable === false ? 'normal' : state, !!o.project?.placeholder, ghost || dimmed(o.id, inst.id), xray, selected, hovered, inst.printable === false);
          selectionBounds.put(mesh, selected, hovered);
          for (const v of o.volumes ?? []) {
            const vm = volumeMeshes.get(`${inst.id}:${v.id}`);
            if (vm) {
              const selected = selectedSurface(s.selected, o.id, inst.id, v.id), hovered = over(o.id, inst.id, v.id);
              vm.material = volumeMaterial(v.type, filamentColor(s, v.extruder || o.extruder), selected, v.seamMode, hovered, outside.has(inst.id), inst.printable === false);
              selectionBounds.put(vm, selected, hovered);
            }
          }
        }
      }
      selectionBounds.end();
    };
    const volumeMeshes = new Map<string, THREE.Mesh>();
    const edgeFade = { value: new THREE.Vector2() };
    const edgeMaterials = new Map<number, THREE.ShaderMaterial>();
    const edgeMaterial = (body: THREE.Color) => {
      const k = body.getHex();
      let m = edgeMaterials.get(k);
      if (!m) {
        m = new THREE.ShaderMaterial({
          uniforms: { color: { value: body.clone().multiplyScalar(0.1) }, fade: edgeFade },
          vertexShader: `#include <common>
#include <clipping_planes_pars_vertex>
varying float vDepth;
void main() {
  #include <begin_vertex>
  #include <project_vertex>
  vDepth = -mvPosition.z;
  #include <clipping_planes_vertex>
}`,
          fragmentShader: `#include <common>
#include <clipping_planes_pars_fragment>
uniform vec3 color;
uniform vec2 fade;
varying float vDepth;
void main() {
  #include <clipping_planes_fragment>
  gl_FragColor = vec4(color, 1.0 - smoothstep(fade.x, fade.y, vDepth));
  #include <colorspace_fragment>
}`,
          transparent: true, depthWrite: false, clipping: true, clippingPlanes: sectionView().ratio > 0 ? [sectionPlane] : null,
        });
        edgeMaterials.set(k, m);
      }
      return m;
    };
    const syncEdges = (camera: THREE.Camera) => {
      const s = getState(), opt = viewOptions();
      const on = opt.edges && !opt.xray && !isPaint(s.gizmo) && s.gizmo !== 'layers';
      const distance = camera.position.distanceTo(controls.target);
      edgeFade.value.set(distance * 2.5, distance * 6);
      for (const mesh of [...meshes.values(), ...volumeMeshes.values()]) {
        const edges = on && mesh.visible ? drawn(mesh.geometry)?.edges : undefined;
        let line = mesh.userData.edgeLine as THREE.LineSegments | undefined;
        const body = mesh.material instanceof THREE.MeshStandardMaterial && !mesh.material.transparent ? mesh.material : null;
        if (!edges || !body) { if (line) line.visible = false; continue; }
        if (line?.geometry !== edges) {
          if (line) mesh.remove(line);
          line = new THREE.LineSegments(edges);
          line.raycast = () => {};
          mesh.add(line);
          mesh.userData.edgeLine = line;
        }
        line.visible = true;
        line.material = edgeMaterial(body.color);
      }
    };
    const drawScene = renderer.render.bind(renderer);
    let captureSize: { width: number; height: number; ratio: number } | null = null;
    renderer.render = (target, camera) => {
      if (target === scene) {
        if (camera === views.camera && captureSize) {
          renderer.setDrawingBufferSize(captureSize.width, captureSize.height, captureSize.ratio);
          captureSize = null;
        } else if (camera !== views.camera && !captureSize) {
          const size = renderer.getSize(new THREE.Vector2());
          captureSize = { width: size.x, height: size.y, ratio: renderer.getPixelRatio() };
          renderer.setDrawingBufferSize(320, Math.max(1, Math.round(320 * size.y / size.x)), 1);
        }
      }
      const reduced: [THREE.Mesh, THREE.BufferGeometry][] = [];
      if (target === scene) {
        lights(camera);
        syncEdges(camera);
        if (!isPaint(getState().gizmo))
          for (const mesh of [...meshes.values(), ...volumeMeshes.values()]) {
            const d = drawn(mesh.geometry), lod = moving ? d?.motion ?? d?.display : d?.display;
            if (lod) { reduced.push([mesh, mesh.geometry]); mesh.geometry = lod; }
          }
      }
      try { drawScene(target, camera); }
      finally { for (const [mesh, geometry] of reduced) mesh.geometry = geometry; }
    };

    const volumeMaterial = (type: VolumeType, color: string, selected: boolean, seam?: SeamMode, hovered = false, outside = false, nonprintable = false) => {
      if (type === 'part') return material(nonprintable ? '#999999' : color, outside && !nonprintable ? 'outside' : 'normal', false, false, false, selected, hovered, nonprintable);
      const k = type + (seam ?? '') + selected + hovered;
      let m = materials.get(k);
      if (!m) {
        const [r, g, b, a] = type === 'seam' ? SEAM_COLOURS[seam ?? 'center'] : VOLUME_COLOURS[type];
        let c = new THREE.Color().setRGB(r, g, b, THREE.SRGBColorSpace);
        if (selected) c = brightenColor(c);
        else if (hovered) c.lerp(new THREE.Color(HOVER_COLOR), 0.25);
        m = new THREE.MeshStandardMaterial({ color: c, roughness: 0.65, metalness: 0.05, transparent: true, opacity: a, depthWrite: false, clippingPlanes: sectionView().ratio > 0 ? [sectionPlane] : null });
        materials.set(k, m);
      }
      return m;
    };
    const sectionPlane = new THREE.Plane();
    let sectionNormal: THREE.Vector3 | null = null, sectionAlign = 0;
    const applySection = () => {
      const sv = sectionView(), box = new THREE.Box3();
      for (const m of meshes.values()) if (m.geometry.boundingBox) box.union(m.geometry.boundingBox.clone().applyMatrix4(m.matrixWorld));
      const on = sv.ratio > 0 && !box.isEmpty();
      if (on && (!sectionNormal || sv.align !== sectionAlign)) {
        sectionNormal = views.camera.getWorldDirection(new THREE.Vector3()).negate();
        sectionAlign = sv.align;
      }
      if (on) {
        const sphere = box.getBoundingSphere(new THREE.Sphere());
        sectionPlane.set(sectionNormal!.clone().negate(), sectionNormal!.dot(sphere.center) + sphere.radius - 2 * sphere.radius * sv.ratio);
      }
      for (const m of [...materials.values(), ...edgeMaterials.values()]) m.clippingPlanes = on ? [sectionPlane] : null;
      render();
    };
    const exploded = (o: PlateObject, inst: Instance, v: Volume) => {
      const m = volumeWorld(o, inst, v), ratio = assembly().on ? assembly().ratio : 1;
      if (ratio === 1) return m;
      const c = instanceBox({ ...o, volumes: [] }, inst).getCenter(new THREE.Vector3());
      const at = new THREE.Vector3().setFromMatrixPosition(m);
      return m.premultiply(new THREE.Matrix4().makeTranslation(...at.sub(c).multiplyScalar(ratio - 1).toArray()));
    };
    const syncObjects = (s: State) => {
      const bounds = new THREE.Box3();
      for (const o of s.objects) for (const i of o.instances) bounds.union(instanceBox(o, i));
      views.setBounds(bounds);
      const seen = new Set<number>();
      outside = outsideBed(s.objects, area, height);
      for (const o of s.objects) {
        for (const inst of o.instances) {
          seen.add(inst.id);
          let mesh = meshes.get(inst.id);
          if (!mesh || mesh.geometry !== o.geometry) {
            if (mesh) scene.remove(mesh);
            mesh = new THREE.Mesh(o.geometry);
            mesh.visible = !needsUpload(o.geometry) || !!drawn(o.geometry);
            mesh.matrixAutoUpdate = false;
            mesh.userData = { object: o.id, instance: inst.id };
            meshes.set(inst.id, mesh);
            scene.add(mesh);
          }
          mesh.matrix.copy(instanceMatrix(o, inst));
          mesh.matrixWorldNeedsUpdate = true;
        }
      }
      if (hovered !== null && !seen.has(hovered)) hovered = null;
      const seenVolumes = new Set<string>();
      for (const o of s.objects)
        for (const inst of o.instances)
          for (const v of o.volumes ?? []) {
            const key = `${inst.id}:${v.id}`;
            seenVolumes.add(key);
            let mesh = volumeMeshes.get(key);
            if (!mesh || mesh.geometry !== v.geometry) {
              if (mesh) scene.remove(mesh);
              mesh = new THREE.Mesh(v.geometry);
              mesh.matrixAutoUpdate = false;
              mesh.userData = { object: o.id, instance: inst.id, volume: v.id };
              volumeMeshes.set(key, mesh);
              scene.add(mesh);
            }
            mesh.renderOrder = v.type === 'part' ? 0 : 1;
            mesh.matrix.copy(exploded(o, inst, v));
            mesh.matrixWorldNeedsUpdate = true;
          }
      for (const [key, mesh] of volumeMeshes)
        if (!seenVolumes.has(key)) {
          scene.remove(mesh);
          volumeMeshes.delete(key);
        }
      tint(s);
      for (const [id, mesh] of meshes)
        if (!seen.has(id)) {
          scene.remove(mesh);
          meshes.delete(id);
        }
      const live = new Set([...s.objects, ...s.projectPlates.flatMap((p) => p.objects)].flatMap((o) => [o.geometry, ...(o.volumes ?? []).map((v) => v.geometry)]));
      for (const g of liveGeometries) if (!live.has(g)) { g.dispose(); const d = drawn(g); d?.display.dispose(); d?.motion?.dispose(); d?.edges?.dispose(); }
      for (const g of live) if (!drawn(g)) requestDisplay(g, render);
      liveGeometries = live;
      syncCut(s);
      syncSlant(s);
      syncGizmo(s);
      syncFlatten(s);
      syncOverlay(s);
      render();
    };

    const cutPlane = new THREE.Mesh(new THREE.PlaneGeometry(1, 1), CUT_PLANE);
    const cutProxy = new THREE.Object3D();
    const cutParts = [THREE.ColorManagement.colorSpaceToWorking(new THREE.Color(0, 1, 1), THREE.SRGBColorSpace),
      THREE.ColorManagement.colorSpaceToWorking(new THREE.Color(1, 0, 1), THREE.SRGBColorSpace)].map((color) => {
      const m = new THREE.Mesh(undefined, new THREE.MeshStandardMaterial({ color, roughness: 0.65, metalness: 0.05, clippingPlanes: [new THREE.Plane()] }));
      m.matrixAutoUpdate = false;
      return m;
    });
    cutPlane.renderOrder = 2;
    scene.add(cutProxy);
    const jointMarks = new THREE.Group();
    const JOINT = new THREE.MeshBasicMaterial({ color: 0xff8c00, side: THREE.DoubleSide, depthTest: false, transparent: true, opacity: 0.85 });
    const JOINT_SELECTED = new THREE.MeshBasicMaterial({ color: 0x00ae42, side: THREE.DoubleSide, depthTest: false, transparent: true, opacity: 0.9 });
    jointMarks.renderOrder = 3;
    let jointKey = '';
    let cutInstance: number | null = null;
    const syncCut = (s: State) => {
      const t = s.gizmo === 'cut' && s.cut ? cutTarget(s) : null;
      const show = !!t;
      if (cutInstance !== null && (!show || cutInstance !== t!.inst.id)) {
        const m = meshes.get(cutInstance);
        if (m) m.visible = true;
        cutInstance = null;
      }
      for (const o of [cutPlane, ...cutParts, jointMarks]) {
        if (show && !o.parent) scene.add(o);
        else if (!show && o.parent) scene.remove(o);
      }
      if (!t) return;
      const mesh = meshes.get(t.inst.id);
      if (mesh) mesh.visible = false;
      cutInstance = t.inst.id;
      const n = cutNormal(s.cut!), p = cutPoint(s, s.cut!)!;
      const size = instanceBox(t.o, t.inst).getSize(new THREE.Vector3());
      const side = 1.5 * Math.max(size.x, size.y, size.z);
      cutPlane.position.copy(p);
      cutPlane.quaternion.setFromUnitVectors(new THREE.Vector3(0, 0, 1), n);
      cutPlane.scale.set(side, side, 1);
      cutParts.forEach((m, k) => {
        m.geometry = t.o.geometry;
        m.matrix.copy(instanceMatrix(t.o, t.inst));
        m.matrixWorldNeedsUpdate = true;
        ((m.material as THREE.MeshStandardMaterial).clippingPlanes![0]).setFromNormalAndCoplanarPoint(k === 0 ? n : n.clone().negate(), p);
      });
      if (!cutDrag) cutProxy.position.copy(p);
      const c = s.cut!;
      const key = JSON.stringify([c.connectors, c.selectedConnectors, c.z, c.rotX, c.rotY, c.mode]);
      if (key !== jointKey) {
        jointKey = key;
        jointMarks.children.forEach((m) => (m as THREE.Mesh).geometry.dispose());
        jointMarks.clear();
        const q = planeRotation(n);
        (c.mode === 'dovetail' ? [] : c.connectors ?? []).forEach(({ pos, a }, i) => {
          const sectors = SECTORS[a.type === 'snap' ? 'circle' : a.shape];
          const m = new THREE.Mesh(new THREE.CircleGeometry(a.size / 2, sectors, Math.PI / 2), c.selectedConnectors?.includes(i) ? JOINT_SELECTED : JOINT);
          m.position.copy(connectorWorld(s, c, pos));
          m.quaternion.copy(q).multiply(new THREE.Quaternion().setFromAxisAngle(new THREE.Vector3(0, 0, 1), -a.angle * Math.PI / 180));
          m.renderOrder = 3;
          jointMarks.add(m);
        });
      }
    };
    const planeHit = (ev: { clientX: number; clientY: number }) => {
      const s = getState(), c = s.cut, t = s.gizmo === 'cut' && c?.editing ? cutTarget(s) : null;
      if (!t || !c) return null;
      setRay(ev);
      const at = ray.ray.intersectPlane(new THREE.Plane().setFromNormalAndCoplanarPoint(cutNormal(c), cutPoint(s, c)!), new THREE.Vector3());
      return at && instanceBox(t.o, t.inst).expandByScalar(0.01).containsPoint(at) ? { s, c, uv: planeUV(s, c, at) } : null;
    };
    let jointDrag: { pointer: number; from: [number, number]; start: CutConnector[] } | null = null;
    const onJointDown = (ev: PointerEvent) => {
      if (ev.button !== 0 && ev.button !== 2) return;
      const hit = planeHit(ev);
      if (!hit) return;
      ev.stopImmediatePropagation();
      ev.preventDefault();
      (document.activeElement as HTMLElement | null)?.blur?.();
      const { c, uv } = hit, list = c.connectors ?? [], at = connectorAt(list, uv), sel = c.selectedConnectors ?? [];
      if (ev.button === 2) {
        if (at >= 0) setCut({ connectors: list.filter((_, k) => k !== at), selectedConnectors: [] });
        return;
      }
      if (at < 0) return setCut({ connectors: [...list, { pos: uv, a: { ...CONNECTOR, ...c.connector } }], selectedConnectors: [list.length] });
      const next = ev.shiftKey ? (sel.includes(at) ? sel.filter((k) => k !== at) : [...sel, at]) : sel.includes(at) ? sel : [at];
      setCut({ selectedConnectors: next, ...(next.length === 1 ? { connector: list[next[0]].a } : {}) });
      if (next.includes(at)) {
        jointDrag = { pointer: ev.pointerId, from: uv, start: list };
        renderer.domElement.setPointerCapture(ev.pointerId);
      }
    };
    const onJointMove = (ev: PointerEvent) => {
      if (!jointDrag || ev.pointerId !== jointDrag.pointer) return;
      const hit = planeHit(ev);
      if (!hit) return;
      ev.stopImmediatePropagation();
      const d = [hit.uv[0] - jointDrag.from[0], hit.uv[1] - jointDrag.from[1]], sel = hit.c.selectedConnectors ?? [];
      setCut({ connectors: jointDrag.start.map((k, i) => (sel.includes(i) ? { ...k, pos: [k.pos[0] + d[0], k.pos[1] + d[1]] as [number, number] } : k)) });
    };
    const onJointUp = (ev: PointerEvent) => {
      if (jointDrag && ev.pointerId === jointDrag.pointer) jointDrag = null;
    };
    renderer.domElement.addEventListener('pointermove', onJointMove, { capture: true });
    renderer.domElement.addEventListener('pointerup', onJointUp, { capture: true });
    renderer.domElement.addEventListener('pointerdown', onJointDown, { capture: true });
    let cutDrag = false;

    const SLANT_COLOR = THREE.ColorManagement.colorSpaceToWorking(new THREE.Color('#FFC43D'), THREE.SRGBColorSpace);
    const SLANT_OVER = THREE.ColorManagement.colorSpaceToWorking(new THREE.Color('#FF7A1A'), THREE.SRGBColorSpace);
    const slantFill = new THREE.MeshBasicMaterial({ color: SLANT_COLOR, transparent: true, opacity: 0.28, side: THREE.DoubleSide, depthWrite: false });
    const slantEdge = new THREE.LineBasicMaterial({ color: SLANT_COLOR, transparent: true, opacity: 0.95, depthTest: false });
    const slantPlane = new THREE.Mesh(new THREE.PlaneGeometry(1, 1), slantFill);
    slantPlane.add(new THREE.LineLoop(new THREE.BufferGeometry().setFromPoints([[-0.5, -0.5], [0.5, -0.5], [0.5, 0.5], [-0.5, 0.5]].map(([x, y]) => new THREE.Vector3(x, y, 0))), slantEdge));
    const slantArrow = new THREE.ArrowHelper(new THREE.Vector3(1, 0, 0), new THREE.Vector3(), 1, SLANT_COLOR.getHex());
    slantPlane.add(slantArrow);
    slantPlane.renderOrder = 2;
    const slantProxy = new THREE.Object3D();
    scene.add(slantProxy);
    let slantDrag = false;
    let slantRadius = 10;
    const syncSlant = (s: State) => {
      const t = s.gizmo === 'slant' && s.slant ? slantTarget(s) : null;
      if (!t) {
        if (slantPlane.parent) scene.remove(slantPlane);
        return;
      }
      if (!slantPlane.parent) scene.add(slantPlane);
      const e = s.slant!, n = slantNormal(e), p = slantPoint(s, e)!;
      const size = instanceBox(t.o, t.inst).getSize(new THREE.Vector3());
      const side = 1.4 * Math.max(size.x, size.y, size.z);
      slantRadius = 0.55 * side;
      slantPlane.position.copy(p);
      slantPlane.quaternion.setFromUnitVectors(new THREE.Vector3(0, 0, 1), n);
      slantPlane.scale.set(side, side, 1);
      const down = new THREE.Vector3(Math.cos(e.dir * Math.PI / 180), Math.sin(e.dir * Math.PI / 180), 0).projectOnPlane(n).normalize();
      slantArrow.setDirection(down.applyQuaternion(slantPlane.quaternion.clone().invert()));
      slantArrow.setLength(0.45, 0.08, 0.05);
      const over = e.limit !== undefined && e.tilt > e.limit + 1e-6;
      const color = over ? SLANT_OVER : SLANT_COLOR;
      slantFill.color.copy(color);
      slantFill.opacity = over ? 0.4 : 0.28;
      slantEdge.color.copy(color);
      slantArrow.setColor(color.getHex());
      if (!slantDrag) slantProxy.position.copy(p).add(new THREE.Vector3(slantRadius + 0.25 * side, 0, 0));
    };

    const proxy = new THREE.Object3D();
    scene.add(proxy);
    const tc = new TransformControls(views.camera, renderer.domElement);
    followers.tc = tc;
    tc.setSize(0.9);
    const tcHelper = tc.getHelper();
    scene.add(tcHelper);
    tc.addEventListener('change', render);
    let rotateStart: { rot: THREE.Quaternion; center: THREE.Vector3; volume?: THREE.Matrix4 } | null = null;
    let endRotate: (() => void) | null = null;
    rotateGizmo = new RotateGizmo(renderer.domElement, () => views.camera, render, {
      start: () => {
        const s = getState(), o = s.objects.find((ob) => ob.id === s.selected?.object);
        const i = o?.instances.find((ii) => ii.id === s.selected?.instance), v = o?.volumes?.find((vv) => vv.id === s.selected?.volume);
        if (!o || !i) return;
        tools.send({ type: 'DRAG' });
        controls.enabled = false;
        endRotate = beginStep('Rotate');
        const world = v ? volumeWorld(o, i, v) : null;
        const DEG = Math.PI / 180;
        rotateStart = { rot: new THREE.Quaternion().setFromEuler(new THREE.Euler(o.rotX * DEG, o.rotY * DEG, i.rotZ * DEG, 'ZYX')),
          center: world ? new THREE.Vector3().setFromMatrixPosition(world) : new THREE.Vector3(i.x, i.y, 0), volume: world ?? undefined };
      },
      change: (axis, angle) => {
        const s = getState(), sel = s.selected, o = s.objects.find((ob) => ob.id === sel?.object);
        const i = o?.instances.find((ii) => ii.id === sel?.instance);
        if (!rotateStart || !sel || !o || !i) return;
        const turn = new THREE.Quaternion().setFromAxisAngle(new THREE.Vector3(axis === 0 ? 1 : 0, axis === 1 ? 1 : 0, axis === 2 ? 1 : 0), angle);
        if (rotateStart.volume && sel.volume !== undefined) {
          const c = rotateStart.center, about = new THREE.Matrix4().makeTranslation(c.x, c.y, c.z).multiply(new THREE.Matrix4().makeRotationFromQuaternion(turn)).multiply(new THREE.Matrix4().makeTranslation(-c.x, -c.y, -c.z));
          updateVolume(o.id, sel.volume, { matrix: instanceMatrix(o, i).invert().multiply(about.multiply(rotateStart.volume)).elements });
          return;
        }
        const e = new THREE.Euler().setFromQuaternion(turn.multiply(rotateStart.rot), 'ZYX'), deg = 180 / Math.PI;
        updateObject(o.id, { rotX: e.x * deg, rotY: e.y * deg });
        updateInstance(o.id, i.id, { rotZ: e.z * deg });
      },
      end: () => {
        endRotate?.(); endRotate = null; rotateStart = null;
        tools.send({ type: 'RELEASE' });
        controls.enabled = true;
        syncGizmo(getState());
      },
    });
    scene.add(rotateGizmo.group);
    let slantStart: THREE.Vector3 | null = null;
    let slantTurning = false;
    const slantRings = new RotateGizmo(renderer.domElement, () => views.camera, render, {
      start: () => {
        const e = getState().slant;
        if (!e) return;
        slantStart = slantNormal(e);
        slantTurning = true;
        controls.enabled = false;
      },
      change: (axis, angle) => {
        if (!slantStart) return;
        const n = slantStart.clone().applyAxisAngle(new THREE.Vector3(axis === 0 ? 1 : 0, axis === 1 ? 1 : 0, axis === 2 ? 1 : 0), angle);
        setSlant(slantAngles(n));
      },
      end: () => {
        slantStart = null;
        slantTurning = false;
        controls.enabled = true;
        syncGizmo(getState());
      },
    });
    scene.add(slantRings.group);
    slantRingsRef = slantRings;
    let dragStart: { x: number; y: number; rotX: number; rotY: number; rotZ: number; scale: [number, number, number];
      volume?: { world: THREE.Matrix4; proxy: THREE.Matrix4 } } | null = null;
    let endStep: (() => void) | null = null;
    tc.addEventListener('dragging-changed', (e) => {
      endStep?.();
      const dragging = (e as unknown as { value: boolean }).value;
      tools.send({ type: dragging ? 'DRAG' : 'RELEASE' });
      controls.enabled = !dragging;
      const s = getState();
      if (s.gizmo === 'slant') {
        slantDrag = dragging;
        endStep = null;
        return;
      }
      if (s.gizmo === 'cut') {
        cutDrag = dragging;
        endStep = null;
        return;
      }
      endStep = dragging ? beginStep(tc.mode === 'translate' ? 'Move' : tc.mode === 'rotate' ? 'Rotate' : 'Scale') : null;
      const o = s.objects.find((ob) => ob.id === s.selected?.object);
      const i = o?.instances.find((ii) => ii.id === s.selected?.instance);
      const v = o?.volumes?.find((vv) => vv.id === s.selected?.volume);
      if (dragging && o && i) {
        dragStart = { x: i.x, y: i.y, rotX: o.rotX, rotY: o.rotY, rotZ: i.rotZ, scale: [...o.scale],
          volume: v && { world: volumeWorld(o, i, v), proxy: proxy.matrix.clone() } };
        proxyRotation(o.rotX, o.rotY, i.rotZ);
        proxy.scale.set(1, 1, 1);
      } else dragStart = null;
    });
    tc.addEventListener('objectChange', () => {
      const s = getState();
      if (s.gizmo === 'cut' && s.cut) {
        setState({ cut: { ...s.cut, z: cutProxy.position.z } });
        return;
      }
      if (s.gizmo === 'slant' && s.slant) {
        const base = slantPoint(s, { ...s.slant, z: 0 });
        if (base) setSlant({ z: slantProxy.position.z - base.z });
        return;
      }
      if (!dragStart || !s.selected) return;
      const { object, instance } = s.selected;
      if (dragStart.volume && s.selected.volume !== undefined) {
        const o = s.objects.find((ob) => ob.id === object)!;
        const i = o.instances.find((ii) => ii.id === instance)!;
        if (tc.mode === 'scale' && s.uniformScale) {
          const k = uniformFactor(proxy.scale);
          proxy.scale.set(k, k, k);
        }
        proxy.updateMatrix();
        const world = proxy.matrix.clone().multiply(dragStart.volume.proxy.clone().invert()).multiply(dragStart.volume.world);
        updateVolume(object, s.selected.volume, { matrix: instanceMatrix(o, i).invert().multiply(world).elements });
        return;
      }
      if (tc.mode === 'translate') updateInstance(object, instance, { x: proxy.position.x, y: proxy.position.y, z: proxy.position.z });
      else if (tc.mode === 'rotate') {
        const deg = 180 / Math.PI;
        updateObject(object, { rotX: dragStart.rotX + proxy.rotation.x * deg, rotY: dragStart.rotY + proxy.rotation.y * deg });
        updateInstance(object, instance, { rotZ: dragStart.rotZ + proxy.rotation.z * deg });
      } else if (tc.mode === 'scale') {
        const k = uniformFactor(proxy.scale);
        const f = proxy.scale.toArray().map((v) => Math.max(0.01, v));
        updateObject(object, { scale: dragStart.scale.map((v, a) => v * (s.uniformScale ? k : f[a])) as [number, number, number] });
      }
    });
    const uniformFactor = (v: THREE.Vector3) => Math.max(0.01, [v.x, v.y, v.z].reduce((a, b) => Math.abs(b - 1) > Math.abs(a - 1) ? b : a, 1));
    const syncGizmo = (s: State) => {
      if (s.gizmo !== 'rotate') rotateGizmo!.detach();
      if (s.gizmo !== 'slant') slantRings.detach();
      if (s.gizmo === 'slant') {
        if (!s.slant || !slantTarget(s)) {
          slantRings.detach();
          return tc.detach();
        }
        if (tc.mode !== 'translate') tc.setMode('translate');
        tc.showX = tc.showY = false;
        tc.showZ = true;
        if (tc.object !== slantProxy) tc.attach(slantProxy);
        if (!slantDrag && !slantTurning) slantRings.attach(slantPoint(s, s.slant)!, slantRadius);
        return;
      }
      if (s.gizmo === 'cut') {
        if (!s.cut || !cutTarget(s)) return tc.detach();
        if (tc.mode !== 'translate') tc.setMode('translate');
        tc.showX = tc.showY = false;
        if (tc.object !== cutProxy) tc.attach(cutProxy);
        return;
      }
      tc.showX = tc.showY = true;
      const o = s.objects.find((ob) => ob.id === s.selected?.object);
      const i = o?.instances.find((ii) => ii.id === s.selected?.instance);
      const v = o?.volumes?.find((vv) => vv.id === s.selected?.volume);
      if (!o || !i || s.gizmo === 'none' || s.gizmo === 'flatten' || s.gizmo === 'measure' || isPaint(s.gizmo) || s.gizmo === 'layers' || s.gizmo === 'brimears' || s.gizmo === 'boolean' || s.gizmo === 'text' || s.gizmo === 'svg' || s.selected?.all || o.project?.placeholder) {
        tc.detach();
        rotateGizmo!.detach();
        return;
      }
      if (s.gizmo === 'rotate') {
        tc.detach();
        const box = v ? new THREE.Box3().setFromObject(volumeMeshes.get(`${i.id}:${v.id}`) ?? meshes.get(i.id)!) : new THREE.Box3().setFromObject(meshes.get(i.id)!);
        rotateGizmo!.attach(box.getCenter(new THREE.Vector3()), box.getBoundingSphere(new THREE.Sphere()).radius);
        return;
      }
      const mode = s.gizmo === 'move' ? 'translate' : s.gizmo;
      if (tc.mode !== mode) tc.setMode(mode);
      tc.showZ = true;
      if (v) {
        if (!dragStart) {
          proxy.position.setFromMatrixPosition(volumeWorld(o, i, v));
          proxy.rotation.set(0, 0, 0, 'XYZ');
          proxy.scale.set(1, 1, 1);
          proxy.updateMatrix();
        }
        if (tc.object !== proxy) tc.attach(proxy);
        return;
      }
      if (!dragStart) {
        const b = new THREE.Box3().setFromObject(meshes.get(i.id)!);
        proxy.position.set(i.x, i.y, mode === 'translate' ? (i.z ?? 0) : (b.min.z + b.max.z) / 2);
        proxyRotation(o.rotX, o.rotY, i.rotZ);
        proxy.scale.set(1, 1, 1);
      }
      if (tc.object !== proxy) tc.attach(proxy);
    };
    const proxyRotation = (rx: number, ry: number, rz: number) => {
      if (tc.mode === 'scale') proxy.rotation.set(rx * Math.PI / 180, ry * Math.PI / 180, rz * Math.PI / 180, 'ZYX');
      else proxy.rotation.set(0, 0, 0, 'XYZ');
    };

    const flattenScene = new THREE.Scene();
    const FLATTEN = new THREE.MeshBasicMaterial({ color: new THREE.Color(0.96, 0.93, 0.93), transparent: true, opacity: 0.5, side: THREE.DoubleSide, depthWrite: true });
    const FLATTEN_HOVER = new THREE.MeshBasicMaterial({ color: 0xffffff, transparent: true, opacity: 0.75, side: THREE.DoubleSide });
    let flatten: { key: string; object: number; instance: number; planes: LayOnFacePlane[] } | null = null;
    let planesFor: { geo: THREE.BufferGeometry; key: string; planes: LayOnFacePlane[] } | null = null;
    let flattenRetry = 0;
    let hoverPlane = -1;
    const clearFlatten = () => {
      for (const m of flattenScene.children as THREE.Mesh[]) m.geometry.dispose();
      flattenScene.clear();
      flatten = null;
      hoverPlane = -1;
    };
    const syncFlatten = (s: State) => {
      const o = s.objects.find((ob) => ob.id === s.selected?.object);
      const i = o?.instances.find((ii) => ii.id === s.selected?.instance);
      if (!o || !i || s.gizmo !== 'flatten' || s.selected?.all || o.project?.placeholder) return clearFlatten();
      const key = `${o.id},${i.id},${o.rotX},${o.rotY},${o.scale},${i.rotZ},${i.x},${i.y},${i.z}`;
      if (flatten?.key === key) return;
      const linearKey = `${o.rotX},${o.rotY},${o.scale},${i.rotZ}`;
      if (planesFor?.geo !== o.geometry || planesFor.key !== linearKey) {
        const pos = o.geometry.attributes.position;
        const points = (o.geometry.userData.hull as Float32Array | undefined) ?? (pos.count <= 50_000 ? pos.array : null);
        if (!points) {
          clearTimeout(flattenRetry);
          flattenRetry = window.setTimeout(() => { flatten = null; syncFlatten(getState()); render(); }, 300);
          return clearFlatten();
        }
        const linear = new THREE.Matrix4().makeRotationFromEuler(new THREE.Euler(o.rotX * Math.PI / 180, o.rotY * Math.PI / 180, i.rotZ * Math.PI / 180, 'ZYX'))
          .multiply(new THREE.Matrix4().makeScale(...o.scale));
        planesFor = { geo: o.geometry, key: linearKey, planes: layOnFacePlanes(points, linear).slice(0, 254) };
      }
      clearFlatten();
      flatten = { key, object: o.id, instance: i.id, planes: planesFor.planes };
      const world = instanceMatrix(o, i);
      const normals = new THREE.Matrix3().getNormalMatrix(world);
      for (const plane of flatten.planes) {
        const c = plane.outline.reduce((a, p) => a.add(p), new THREE.Vector3()).divideScalar(plane.outline.length);
        const lift = plane.normal.clone().applyMatrix3(normals).normalize().multiplyScalar(0.1);
        const pts = plane.outline.map((p) => p.clone().multiplyScalar(0.9).addScaledVector(c, 0.1).applyMatrix4(world).add(lift));
        const geo = new THREE.BufferGeometry().setFromPoints(pts);
        geo.setIndex(pts.slice(2).flatMap((_, k) => [0, k + 1, k + 2]));
        flattenScene.add(new THREE.Mesh(geo, FLATTEN));
      }
    };
    const pickPlane = (ev: { clientX: number; clientY: number }) => {
      if (!flatten) return -1;
      setRay(ev);
      const hit = ray.intersectObjects(flattenScene.children, false)[0];
      return hit ? flattenScene.children.indexOf(hit.object) : -1;
    };
    const setHoverPlane = (k: number) => {
      if (k === hoverPlane) return;
      (flattenScene.children as THREE.Mesh[]).forEach((m, j) => (m.material = j === k ? FLATTEN_HOVER : FLATTEN));
      hoverPlane = k;
      render();
    };
    gizmoRef.current = () => syncGizmo(getState());

    const measuring = new MeasureView(el, () => views.camera);
    const measurable = (s: State, instance: number) => s.objects.some((o) => !o.project?.placeholder && o.instances.some((i) => i.id === instance &&
      isSelected(s.selected, o.id, i.id)));
    const unsubMeasure = subscribeMeasure(() => { measuring.sync(); render(); });

    const ray = new THREE.Raycaster();
    const ndc = new THREE.Vector2();
    const setRay = (ev: { clientX: number; clientY: number }) => {
      const r = renderer.domElement.getBoundingClientRect();
      ndc.set(((ev.clientX - r.left) / r.width) * 2 - 1, -((ev.clientY - r.top) / r.height) * 2 + 1);
      ray.setFromCamera(ndc, views.camera);
    };
    const EXACT_TRIANGLES = 100_000;
    const box = new THREE.Box3();
    const entry = new THREE.Vector3();
    const inverse = new THREE.Matrix4();
    const linear = new THREE.Matrix3();
    const origin = new THREE.Vector3(), direction = new THREE.Vector3();
    const rayOrigin = [0, 0, 0], rayDirection = [0, 0, 0];
    const hits: { mesh: THREE.Mesh; distance: number }[] = [];
    const triHit = { tri: -1 };
    type Hit = { object: THREE.Object3D; distance: number; tri?: number; local?: THREE.Vector3 };
    const pick = (ev: { clientX: number; clientY: number }): Hit | undefined => {
      setRay(ev);
      hits.length = 0;
      for (const mesh of meshes.values()) {
        box.copy(mesh.geometry.boundingBox!).applyMatrix4(mesh.matrixWorld);
        if (ray.ray.intersectBox(box, entry)) hits.push({ mesh, distance: entry.distanceTo(ray.ray.origin) });
      }
      hits.sort((a, b) => a.distance - b.distance);
      let best: Hit | undefined;
      for (const { mesh, distance } of hits) {
        if (best && distance >= best.distance) break;
        const g = mesh.geometry;
        const bvh = g.userData.bvh as Bvh | undefined;
        let hit: Hit | undefined;
        if (bvh) {
          inverse.copy(mesh.matrixWorld).invert();
          origin.copy(ray.ray.origin).applyMatrix4(inverse);
          direction.copy(ray.ray.direction).applyMatrix3(linear.setFromMatrix4(inverse));
          const t = raycastBvh(bvh, g.attributes.position.array, g.index!.array, origin.toArray(rayOrigin), direction.toArray(rayDirection), best?.distance, triHit);
          if (t !== Infinity) hit = { object: mesh, distance: t, tri: triHit.tri, local: origin.clone().addScaledVector(direction, t) };
        } else {
          const triangles = (g.index ? g.index.count : g.attributes.position.count) / 3;
          const h = triangles > EXACT_TRIANGLES ? undefined : ray.intersectObject(mesh, false)[0];
          hit = !h ? (triangles > EXACT_TRIANGLES ? { object: mesh, distance } : undefined)
            : { object: mesh, distance: h.distance, tri: h.faceIndex ?? undefined, local: mesh.worldToLocal(h.point.clone()) };
        }
        if (hit && (!best || hit.distance < best.distance)) best = hit;
      }
      hits.length = 0;
      return best;
    };
    const bedPoint = () => {
      const p = new THREE.Vector3();
      return ray.ray.intersectPlane(new THREE.Plane(new THREE.Vector3(0, 0, 1), 0), p) ? p : null;
    };
    let drag: { object: number; instance: number; dx: number; dy: number; x: number; y: number; group: { object: number; instance: number; x: number; y: number }[]; moved: boolean } | null = null;
    let rect: { x: number; y: number; remove: boolean } | null = null;
    const rectEl = document.createElement('div');
    rectEl.className = 'canvas-rect';
    rectEl.hidden = true;
    el.appendChild(rectEl);
    const tower = new TowerView();
    scene.add(tower.group);
    let towerDrag: { dx: number; dy: number; moved: boolean } | null = null;
    let downAt: { x: number; y: number } | null = null;
    const touches = new Set<number>();
    const onTouchDown = (ev: PointerEvent) => {
      if (ev.pointerType !== 'touch') return;
      touches.add(ev.pointerId);
      if (touches.size > 1) {
        drag = null;
        downAt = null;
        controls.enabled = true;
      }
    };
    const onTouchUp = (ev: PointerEvent) => touches.delete(ev.pointerId);
    renderer.domElement.addEventListener('pointerdown', onTouchDown, { capture: true });
    window.addEventListener('pointerup', onTouchUp);
    window.addEventListener('pointercancel', onTouchUp);
    const onDown = (ev: PointerEvent) => {
      if (ev.button === 2) { rightAt = { x: ev.clientX, y: ev.clientY, moved: false }; rightTime = performance.now(); }
      if (ev.button !== 0 || touches.size > 1 || (tc.dragging || (tc as unknown as { axis: string | null }).axis)) return;
      if (ev.pointerType === 'touch') return;
      const k = pickPlane(ev);
      if (k >= 0 && flatten) {
        const o = getState().objects.find((ob) => ob.id === flatten!.object);
        if (o) {
          const next = layOnFace(o, flatten.instance, flatten.planes[k].normal);
          updateObject(o.id, { rotX: next.rotX, rotY: next.rotY, instances: next.instances }, 'Lay on face');
        }
        return;
      }
      downAt = { x: ev.clientX, y: ev.clientY };
      const hit = pick(ev);
      if (topSurfacesShown()) {
        const sf = hit && surfaceAt((hit.object.userData as { instance: number }).instance, hit.tri);
        selectSurface(sf ? sf.inst.instanceId : null, sf ? sf.region : -1);
        render();
      }
      const towerHit = tower.hit(ray);
      if (towerHit && (!hit || towerHit.distance < hit.distance)) {
        const p = bedPoint(), at = tower.corner(getState());
        if (p && at && getState().gizmo !== 'measure') {
          towerDrag = { dx: at[0] - p.x, dy: at[1] - p.y, moved: false };
          controls.enabled = false;
          renderer.domElement.setPointerCapture(ev.pointerId);
        }
        return;
      }
      if (!hit) {
        if ((ev.shiftKey || ev.altKey) && getState().gizmo !== 'measure') {
          rect = { x: ev.clientX, y: ev.clientY, remove: ev.altKey };
          controls.enabled = false;
          renderer.domElement.setPointerCapture(ev.pointerId);
        }
        return;
      }
      const { object, instance } = hit.object.userData as { object: number; instance: number };
      if ((ev.ctrlKey || ev.metaKey || ev.shiftKey) && getState().gizmo !== 'measure') return toggleSelection(object, [instance]);
      const s0 = getState();
      if (!isSelected(s0.selected, object, instance) || s0.selected?.volume !== undefined) setState({ selected: { object, instance } });
      if (getState().gizmo === 'measure') return;
      const p = bedPoint();
      const s = getState(), o = s.objects.find((ob) => ob.id === object)!;
      const i = o.instances.find((ii) => ii.id === instance)!;
      const group = s.objects.filter((ob) => !ob.project?.placeholder).flatMap((ob) => ob.instances.filter((ii) => isSelected(s.selected, ob.id, ii.id)).map((ii) => ({ object: ob.id, instance: ii.id, x: ii.x, y: ii.y })));
      if (p && !o.project?.placeholder) drag = { object, instance, dx: i.x - p.x, dy: i.y - p.y, x: i.x, y: i.y, group, moved: false };
      controls.enabled = false;
      renderer.domElement.setPointerCapture(ev.pointerId);
    };
    const onMove = (ev: PointerEvent) => {
      if (rect) {
        const r = el.getBoundingClientRect();
        Object.assign(rectEl.style, { left: `${Math.min(rect.x, ev.clientX) - r.left}px`, top: `${Math.min(rect.y, ev.clientY) - r.top}px`,
          width: `${Math.abs(ev.clientX - rect.x)}px`, height: `${Math.abs(ev.clientY - rect.y)}px` });
        rectEl.hidden = false;
        return;
      }
      if (towerDrag) {
        setRay(ev);
        const p = bedPoint();
        if (!p) return;
        towerDrag.moved = true;
        tower.dragAt = [p.x + towerDrag.dx, p.y + towerDrag.dy];
        tower.sync(getState());
        render();
        return;
      }
      if (!drag) return;
      setRay(ev);
      const p = bedPoint();
      if (!p) return;
      if (!drag.moved) endStep = beginStep('Move');
      drag.moved = true;
      const ddx = p.x + drag.dx - drag.x, ddy = p.y + drag.dy - drag.y;
      const positions = new Map(drag.group.map((g) => [g.instance, { x: g.x + ddx, y: g.y + ddy }]));
      moving = true;
      clearTimeout(settle);
      setState((s) => ({ objects: s.objects.map((o) => o.instances.some((i) => positions.has(i.id)) ? { ...o,
        instances: o.instances.map((i) => positions.has(i.id) ? { ...i, ...positions.get(i.id)! } : i),
      } : o), job: markStale(s.job) }));
    };
    const onUp = (ev: PointerEvent) => {
      if (rect) {
        const s = getState(), r = renderer.domElement.getBoundingClientRect();
        const x0 = Math.min(rect.x, ev.clientX), x1 = Math.max(rect.x, ev.clientX), y0 = Math.min(rect.y, ev.clientY), y1 = Math.max(rect.y, ev.clientY);
        const inside = new Set<string>();
        if (x1 - x0 >= 4 || y1 - y0 >= 4)
          for (const o of s.objects) for (const i of o.instances) {
            const v = instanceBox(o, i).getCenter(new THREE.Vector3()).project(views.camera);
            const x = r.left + (v.x + 1) / 2 * r.width, y = r.top + (1 - v.y) / 2 * r.height;
            if (v.z < 1 && x >= x0 && x <= x1 && y >= y0 && y <= y1) inside.add(`${o.id}:${i.id}`);
          }
        const now = s.objects.flatMap((o) => o.instances.filter((i) => isSelected(s.selected, o.id, i.id)).map((i) => `${o.id}:${i.id}`));
        if (inside.size) setState({ selected: selectionOf(s, rect.remove ? now.filter((k) => !inside.has(k)) : [...new Set([...now, ...inside])]) });
        rect = null;
        rectEl.hidden = true;
        controls.enabled = true;
        downAt = null;
        return;
      }
      if (!drag && downAt && Math.hypot(ev.clientX - downAt.x, ev.clientY - downAt.y) < 4 && !tc.dragging && !pick(ev)) {
        const k = grid.plateAt(getState(), bedPoint());
        if (k !== null && k !== getState().plateIndex) selectPlate(k);
        else setState({ selected: null });
      }
      if (drag?.moved) {
        moving = false;
        render();
        endStep?.();
        endStep = null;
        const s = getState(), o = s.objects.find((ob) => ob.id === drag!.object), i = o?.instances.find((ii) => ii.id === drag!.instance);
        const k = i && canAddPlate(s) ? grid.plateAt(s, new THREE.Vector3(i.x, i.y, 0)) : null;
        if (o && k !== null && k !== s.plateIndex) moveToPlate([...new Set(drag.group.map((g) => g.object))], k, plateOffset(s, area, k));
      }
      if (towerDrag?.moved && tower.dragAt) {
        const t = getState().wipeTower, at = t ? TowerView.inBed(tower.dragAt, t, parsePoints(printerPreset(getState())?.config.printable_area)) : tower.dragAt;
        undoable('Move', () => setState((st) => ({ wipeTowerPos: { ...st.wipeTowerPos, [st.plateIndex]: at }, job: markStale(st.job) })));
      }
      tower.dragAt = null;
      towerDrag = null;
      drag = null;
      downAt = null;
      controls.enabled = true;
    };
    const onMeasureDown = (ev: PointerEvent) => {
      if (ev.button !== 0 || getState().gizmo !== 'measure' || !measuring.click()) return;
      ev.stopImmediatePropagation();
      ev.preventDefault();
    };
    renderer.domElement.addEventListener('pointerdown', onMeasureDown, { capture: true });

    const paintView = new PaintView();
    scene.add(paintView.group);
    let stroke: { object: number; instance: number; volume?: number; kind: PaintKind; state: number; paint: Painting | null; pointer: number; at: { clientX: number; clientY: number }; world: THREE.Vector3 | null } | null = null;
    type PaintHit = Hit & { volume?: number };
    const paintTarget = (s: State, object: number, volume?: number) => {
      const o = s.objects.find((ob) => ob.id === object);
      return volume === undefined ? o : o?.volumes?.find((v) => v.id === volume && v.type === 'part');
    };
    const paintPick = (ev: { clientX: number; clientY: number }): PaintHit | undefined => {
      const s = getState();
      let best: PaintHit | undefined = pick(ev);
      for (const mesh of volumeMeshes.values()) {
        const ud = mesh.userData as { object: number; instance: number; volume: number };
        if (ud.object !== s.selected?.object || !paintTarget(s, ud.object, ud.volume)) continue;
        const h = ray.intersectObject(mesh, false)[0];
        if (h && h.faceIndex != null && (!best || h.distance < best.distance))
          best = { object: mesh, distance: h.distance, tri: h.faceIndex, local: mesh.worldToLocal(h.point.clone()), volume: ud.volume };
      }
      return best;
    };
    const paintAt = (hit: PaintHit, view: THREE.Vector3) => {
      const s = getState();
      const target = paintTarget(s, stroke!.object, stroke!.volume);
      if (!target || hit.tri === undefined || !hit.local) return;
      const t = paintTool();
      const world = hit.object.matrixWorld;
      const allow = (stroke!.kind === 'support' && t.overhangsOnly && overhangFacets(target.geometry, new THREE.Matrix3().getNormalMatrix(world), t.highlight)) || undefined;
      stroke!.paint = t.tool === 'fill' && stroke!.kind === 'color' ? bucketFill(target.geometry, stroke!.paint, hit.tri, t.edges ? t.angle : -1, stroke!.state)
        : t.tool === 'fill' ? fillPaint(target.geometry, stroke!.paint, hit.tri, t.angle, stroke!.state, allow)
        : t.tool === 'gap' ? stroke!.paint
        : t.tool === 'triangle' ? paintTriangle(stroke!.paint, hit.tri, stroke!.state)
        : t.tool === 'height' ? heightRange(target.geometry, stroke!.paint, hit.tri, world, hit.local.clone().applyMatrix4(world).z, t.height, stroke!.state)
        : brush(target.geometry, stroke!.paint, hit.tri, hit.local, t.radius / world.getMaxScaleOnAxis(), t.tool, view, stroke!.state, allow);
      paintView.sync(s, { volume: stroke!.volume, paint: stroke!.paint });
      render();
    };
    const viewDir = (mesh: THREE.Object3D) => ray.ray.direction.clone().transformDirection(mesh.matrixWorld.clone().invert());
    const earMarks = new THREE.Group();
    const EAR = new THREE.MeshBasicMaterial({ color: 0x6ba6d8, transparent: true, opacity: 0.85, depthTest: false });
    const EAR_SELECTED = new THREE.MeshBasicMaterial({ color: 0xe8862c, transparent: true, opacity: 0.9, depthTest: false });
    earMarks.renderOrder = 3;
    scene.add(earMarks);
    const syncEars = () => {
      earMarks.children.forEach((m) => (m as THREE.Mesh).geometry.dispose());
      earMarks.clear();
      const s = getState(), o = s.gizmo === 'brimears' ? s.objects.find((x) => x.id === s.selected?.object) : undefined;
      const inst = o?.instances.find((i) => i.id === s.selected?.instance);
      if (!o || !inst) return render();
      const world = instanceMatrix(o, inst), sel = earsUi().selected;
      (o.brimEars ?? []).forEach((e, i) => {
        const m = new THREE.Mesh(new THREE.CylinderGeometry(e.r, e.r, 0.4, 32).rotateX(Math.PI / 2), sel.includes(i) ? EAR_SELECTED : EAR);
        m.position.set(...e.p).applyMatrix4(world);
        m.renderOrder = 3;
        earMarks.add(m);
      });
      render();
    };
    const earAt = (ev: { clientX: number; clientY: number }) => {
      const s = getState(), o = s.gizmo === 'brimears' ? s.objects.find((x) => x.id === s.selected?.object) : undefined;
      const inst = o?.instances.find((i) => i.id === s.selected?.instance);
      if (!o || !inst) return null;
      const hit = pick(ev), world = instanceMatrix(o, inst);
      const onObject = hit && (hit.object.userData as { instance?: number }).instance === inst.id ? hit : null;
      const point = onObject?.local ? onObject.local.clone().applyMatrix4(world) : null;
      setRay(ev);
      const probe = point ?? ray.ray.intersectPlane(new THREE.Plane(new THREE.Vector3(0, 0, 1), 0), new THREE.Vector3());
      let ear = -1, best = Infinity;
      (o.brimEars ?? []).forEach((e, i) => {
        const c = new THREE.Vector3(...e.p).applyMatrix4(world), d = probe ? Math.hypot(c.x - probe.x, c.y - probe.y) : Infinity;
        if (d <= e.r && d < best) [ear, best] = [i, d];
      });
      return { o, local: onObject?.local ?? null, ear };
    };
    const onEarsDown = (ev: PointerEvent) => {
      if (ev.button !== 0 && ev.button !== 2) return;
      const at = earAt(ev);
      if (!at || (at.ear < 0 && (!at.local || ev.button === 2))) return;
      ev.stopImmediatePropagation();
      ev.preventDefault();
      const ears = at.o.brimEars ?? [], sel = earsUi().selected;
      if (ev.button === 2) {
        setEars(at.o.id, ears.filter((_, i) => i !== at.ear), 'Delete brim ear');
        setEarsUi({ selected: [] });
      } else if (at.ear >= 0) setEarsUi({ selected: ev.shiftKey ? (sel.includes(at.ear) ? sel.filter((k) => k !== at.ear) : [...sel, at.ear]) : [at.ear] });
      else {
        setEars(at.o.id, [...ears, { p: [at.local!.x, at.local!.y, at.local!.z], r: earsUi().radius || defaultEarRadius() }], 'Add brim ear');
        setEarsUi({ selected: [ears.length] });
      }
    };
    const onEarsWheel = (ev: WheelEvent) => {
      if (getState().gizmo !== 'brimears' || !(ev.ctrlKey || ev.metaKey)) return;
      ev.preventDefault();
      ev.stopImmediatePropagation();
      const s = getState(), o = s.objects.find((x) => x.id === s.selected?.object), u = earsUi();
      const radius = Math.min(20, Math.max(0.1, +((u.radius || defaultEarRadius()) + (ev.deltaY < 0 ? 0.1 : -0.1)).toFixed(2)));
      setEarsUi({ radius });
      if (o && u.selected.length) setEars(o.id, (o.brimEars ?? []).map((e, i) => (u.selected.includes(i) ? { ...e, r: radius } : e)), 'Change brim ear radius');
    };
    renderer.domElement.addEventListener('pointerdown', onEarsDown, { capture: true });
    const onTextDown = (ev: PointerEvent) => {
      const s = getState();
      if (ev.button !== 0 || (s.gizmo !== 'text' && s.gizmo !== 'svg') || !textPlacing()) return;
      const hit = pick(ev);
      if (!hit?.local || hit.tri === undefined || (hit.object.userData as { object: number }).object !== s.selected?.object) return;
      ev.stopImmediatePropagation();
      ev.preventDefault();
      const g = (hit.object as THREE.Mesh).geometry, pos = g.attributes.position, idx = g.index;
      const v = [0, 1, 2].map((k) => new THREE.Vector3().fromBufferAttribute(pos, idx ? idx.getX(hit.tri! * 3 + k) : hit.tri! * 3 + k));
      const n = v[1].clone().sub(v[0]).cross(v[2].clone().sub(v[0])).normalize();
      setTextPlacing(false);
      const at: [number, number, number] = [hit.local.x, hit.local.y, hit.local.z];
      void (s.gizmo === 'svg' ? import('./svg') : import('./text')).then((m) => m.placeOnSurface(at, [n.x, n.y, n.z]));
    };
    renderer.domElement.addEventListener('pointerdown', onTextDown, { capture: true });
    renderer.domElement.addEventListener('wheel', onEarsWheel, { capture: true, passive: false });
    const unsubEars = subscribeEarsUi(syncEars);

    const onPaintDown = (ev: PointerEvent) => {
      const s = getState();
      if (!isPaint(s.gizmo) || (ev.button !== 0 && ev.button !== 2) || touches.size > 1) return;
      if (s.gizmo === 'color' && ev.button === 2) return;
      const state = ev.button === 2 ? 2 : ev.shiftKey ? 0 : s.gizmo === 'color' ? paintTool().filament : 1;
      const hit = paintPick(ev);
      const id = hit?.object.userData as { object: number; instance: number } | undefined;
      if (!hit || !id) return;
      ev.stopImmediatePropagation();
      ev.preventDefault();
      if (id.object !== s.selected?.object || s.gizmo === 'fuzzy' && state === 2 || paintTool().tool === 'gap') return;
      const target = paintTarget(s, id.object, hit.volume)!;
      stroke = { object: id.object, instance: id.instance, volume: hit.volume, kind: s.gizmo, state, paint: paintOf(target, s.gizmo), pointer: ev.pointerId, at: { clientX: ev.clientX, clientY: ev.clientY }, world: null };
      controls.enabled = false;
      renderer.domElement.setPointerCapture(ev.pointerId);
      paintAt(hit, viewDir(hit.object));
      if (stroke && hit.local) stroke.world = hit.local.clone().applyMatrix4(hit.object.matrixWorld);
    };
    const onPaintMove = (ev: PointerEvent) => {
      const s = getState();
      if (!isPaint(s.gizmo)) return;
      const hit = paintPick(ev);
      const id = hit?.object.userData as { object: number; instance: number } | undefined;
      const onSelected = !!hit && id?.object === s.selected?.object && !!hit.local;
      if (onSelected && hit!.tri !== undefined) {
        const mesh = hit!.object as THREE.Mesh, g = (paintTarget(s, id!.object, hit!.volume) ?? s.objects.find((ob) => ob.id === id!.object)!).geometry;
        const idx = g.index, p = g.attributes.position;
        const c = (k: number) => new THREE.Vector3().fromBufferAttribute(p, idx ? idx.getX(3 * hit!.tri! + k) : 3 * hit!.tri! + k).applyMatrix4(mesh.matrixWorld);
        const a = c(0), b = c(1), d = c(2);
        const normal = d.sub(b).cross(a.clone().sub(b)).normalize();
        paintView.showCursor(hit!.local!.clone().applyMatrix4(mesh.matrixWorld), normal, ['circle', 'sphere'].includes(paintTool().tool) ? paintTool().radius : 0);
      } else paintView.showCursor(null);
      if (stroke && ev.pointerId === stroke.pointer) {
        ev.stopImmediatePropagation();
        const from = stroke.at, to = { clientX: ev.clientX, clientY: ev.clientY };
        const end = onSelected && id!.instance === stroke.instance && hit!.volume === stroke.volume ? hit!.local!.clone().applyMatrix4(hit!.object.matrixWorld) : null;
        const steps = ['circle', 'sphere'].includes(paintTool().tool) && end && stroke.world ? Math.min(64, Math.ceil(end.distanceTo(stroke.world) / (paintTool().radius / 2))) : 1;
        for (let k = 1; k <= steps; k++) {
          const at = k === steps ? to : { clientX: from.clientX + (to.clientX - from.clientX) * k / steps, clientY: from.clientY + (to.clientY - from.clientY) * k / steps };
          const h = k === steps ? hit : paintPick(at);
          const hid = h?.object.userData as { object: number; instance: number } | undefined;
          if (h?.local && hid?.object === stroke.object && hid.instance === stroke.instance && h.volume === stroke.volume) paintAt(h, viewDir(h.object));
        }
        stroke.at = to;
        stroke.world = end;
      }
      render();
    };
    const onPaintUp = (ev: PointerEvent) => {
      if (!stroke || ev.pointerId !== stroke.pointer) return;
      ev.stopImmediatePropagation();
      const { object, volume, kind, paint, state } = stroke;
      stroke = null;
      controls.enabled = true;
      const target = paintTarget(getState(), object, volume);
      const step = kind !== 'color' ? PAINT_STEP[kind] : state ? `Painted using: Filament ${state}` : 'Remove painted color';
      if (target && paint !== paintOf(target, kind)) setPaint(object, kind, paint, step, volume);
      else paintView.sync(getState());
    };
    const onPaintWheel = (ev: WheelEvent) => {
      if (!isPaint(getState().gizmo) || !(ev.ctrlKey || ev.metaKey)) return;
      ev.preventDefault();
      ev.stopImmediatePropagation();
      const t = paintTool(), up = ev.deltaY < 0;
      if (t.tool === 'fill') setPaintTool({ angle: t.angle + (up ? 5 : -5) });
      else if (t.tool === 'height') setPaintTool({ height: t.height + (up ? HEIGHT.step : -HEIGHT.step) });
      else setPaintTool({ radius: t.radius + (up ? RADIUS.step : -RADIUS.step) });
    };
    renderer.domElement.addEventListener('pointerdown', onPaintDown, { capture: true });
    renderer.domElement.addEventListener('pointermove', onPaintMove, { capture: true });
    renderer.domElement.addEventListener('pointerup', onPaintUp, { capture: true });
    renderer.domElement.addEventListener('pointercancel', onPaintUp, { capture: true });
    renderer.domElement.addEventListener('wheel', onPaintWheel, { capture: true, passive: false });
    const unsubPaintTool = subscribePaintTool(() => { paintView.sync(getState()); render(); });
    renderer.domElement.addEventListener('pointerdown', onDown);
    renderer.domElement.addEventListener('pointermove', onMove);
    renderer.domElement.addEventListener('pointerup', onUp);
    renderer.domElement.addEventListener('pointercancel', onUp);

    let hoverAt: { clientX: number; clientY: number } | null = null;
    let hoverFrame = 0;
    let hoverShift = false;
    let orbiting = false;
    const hoverMs: number[] = [];
    const setHover = (id: number | null, volume?: number) => {
      if (id === hovered && volume === hoveredVolume) return;
      hovered = id; hoveredVolume = volume;
      const object = id === null ? undefined : (meshes.get(id)?.userData as { object?: number } | undefined)?.object;
      if (object !== undefined) setHoverLink({ object, instance: id!, source: 'canvas' });
      else if (hoverLink()?.source === 'canvas') setHoverLink(null);
      tint(getState());
      render();
    };
    const hoverPass = () => {
      hoverFrame = 0;
      if (!hoverAt || orbiting) return;
      const t0 = performance.now();
      const axis = (tc as unknown as { axis: string | null }).axis;
      const hit = axis ? undefined : pick(hoverAt);
      if (hoverMs.length > 4096) hoverMs.splice(0, 2048);
      hoverMs.push(performance.now() - t0);
      setHover(hit ? (hit.object.userData as { instance: number }).instance : null, hit?.object.userData.volume);
      if (topSurfacesShown()) {
        const sf = hit && surfaceAt((hit.object.userData as { instance: number }).instance, hit.tri);
        if (hoverSurface(sf ? sf.inst.instanceId : null, sf ? sf.region : -1)) render();
      }
      planarTip(hit, hoverAt);
      if (flatten) setHoverPlane(pickPlane(hoverAt));
      if (getState().gizmo === 'measure') {
        const r = renderer.domElement.getBoundingClientRect();
        const id = hit && (hit.object.userData as { instance: number }).instance;
        measuring.hover(hit?.tri !== undefined && hit.local && measurable(getState(), id!) ? { mesh: hit.object as THREE.Mesh, tri: hit.tri, local: hit.local } : null,
          new THREE.Vector2(hoverAt.clientX - r.left, hoverAt.clientY - r.top), hoverShift);
      }
    };
    const onHover = (ev: PointerEvent) => {
      if (ev.buttons || orbiting || tc.dragging) {
        hoverAt = null;
        setHover(null);
        tip.hidden = true;
        return;
      }
      hoverAt = { clientX: ev.clientX, clientY: ev.clientY };
      hoverShift = ev.shiftKey;
      if (!hoverFrame) hoverFrame = requestAnimationFrame(hoverPass);
    };
    const onLeave = () => {
      hoverAt = null;
      if (hoverSurface(null)) render();
      setHover(null);
      tip.hidden = true;
      setHoverPlane(-1);
      if (measureState().hover) setMeasure({ ...measureState(), hover: undefined });
    };

    let rightAt: { x: number; y: number; moved: boolean } | null = null, rightTime = -Infinity;
    const openMenu = (ev: MouseEvent) => {
      const hit = pick(ev);
      if (!hit) {
        const k = grid.plateAt(getState(), bedPoint());
        if (k !== null && k !== getState().plateIndex) selectPlate(k);
      }
      const s = getState();
      if (!hit) return setState({ contextMenu: { x: ev.clientX, y: ev.clientY } });
      const { object, instance } = hit.object.userData as { object: number; instance: number };
      const inSelection = isSelected(s.selected, object, instance);
      setState({ ...(inSelection ? {} : { selected: { object, instance } }), contextMenu: { x: ev.clientX, y: ev.clientY, object } });
    };
    const onRightMove = (ev: PointerEvent) => {
      if (rightAt && !rightAt.moved && Math.hypot(ev.clientX - rightAt.x, ev.clientY - rightAt.y) > 4) rightAt.moved = true;
    };
    const onRightUp = (ev: PointerEvent) => {
      if (ev.button !== 2 || !rightAt) return;
      const press = rightAt;
      rightAt = null;
      rightTime = performance.now();
      if (press.moved || Math.hypot(ev.clientX - press.x, ev.clientY - press.y) > 4 || ev.target !== renderer.domElement) return;
      openMenu(ev);
    };
    const onContextMenu = (ev: MouseEvent) => {
      ev.preventDefault();
      if (rightAt || performance.now() - rightTime < 400) return;
      const r = renderer.domElement.getBoundingClientRect(), selected = getState().selected;
      setState({ contextMenu: { x: ev.clientX || r.left + r.width / 2, y: ev.clientY || r.top + r.height / 2,
        object: selected?.object, instance: selected?.instance, volume: selected?.volume } });
    };
    window.addEventListener('pointermove', onRightMove);
    window.addEventListener('pointerup', onRightUp);
    renderer.domElement.dataset.customContext = 'orca';
    renderer.domElement.addEventListener('contextmenu', onContextMenu);
    controls.addEventListener('start', () => {
      orbiting = true;
      onLeave();
    });
    controls.addEventListener('end', () => (orbiting = false));
    renderer.domElement.addEventListener('pointermove', onHover);
    const redraw = () => { if (!document.hidden) render(); };
    renderer.domElement.addEventListener('webglcontextrestored', redraw);
    document.addEventListener('visibilitychange', redraw);
    renderer.domElement.addEventListener('pointerleave', onLeave);

    const resize = () => {
      const w = el.clientWidth, h = el.clientHeight;
      if (!w || !h) return;
      const size = renderer.getSize(new THREE.Vector2());
      const ratio = pixelRatio(w, h);
      if (ratio !== renderer.getPixelRatio()) renderer.setPixelRatio(ratio);
      if (size.x !== w || size.y !== h || renderer.domElement.width !== Math.floor(w * ratio)) renderer.setSize(w, h);
      views.resize(w, h);
      measuring.resize(w, h);
      render();
    };
    const ro = new ResizeObserver(resize);
    ro.observe(el);
    const unwatchRatio = watchPixelRatio(resize);

    const overlayRoot = new THREE.Group();
    scene.add(overlayRoot);
    const css = (v: string) => getComputedStyle(document.documentElement).getPropertyValue(v).trim() || '#FFC43D';
    const topMat = new THREE.MeshBasicMaterial({ polygonOffset: true, polygonOffsetFactor: -2, polygonOffsetUnits: -4 });
    const ledgeMat = new THREE.LineBasicMaterial();
    const overlayColours = () => {
      topMat.color.set(css('--accent'));
      ledgeMat.color.set(css('--accent-hover'));
    };
    overlayColours();
    const overlays = new Map<number, { group: THREE.Group; ov: InstanceOverlay }>();
    const buildOverlay = () => {
      for (const { group } of overlays.values()) {
        for (const c of group.children as (THREE.Mesh | THREE.LineSegments)[]) c.geometry.dispose();
        overlayRoot.remove(group);
      }
      overlays.clear();
      for (const ov of getPatchPreview()?.overlays ?? []) {
        const group = new THREE.Group();
        group.matrixAutoUpdate = false;
        if (ov.top.length) {
          const g = new THREE.BufferGeometry().setAttribute('position', new THREE.BufferAttribute(ov.top, 3));
          group.add(new THREE.Mesh(g, topMat));
        }
        if (ov.ledges.length) {
          const g = new THREE.BufferGeometry().setAttribute('position', new THREE.BufferAttribute(ov.ledges, 3)).translate(0, 0, -0.15);
          group.add(new THREE.LineSegments(g, ledgeMat));
        }
        group.userData.from = ov.at.clone().invert();
        overlays.set(ov.instanceId, { group, ov });
        overlayRoot.add(group);
      }
      syncOverlay(getState());
      render();
    };
    const syncOverlay = (s: State) => {
      overlayRoot.visible = nonplanarViewShown() && !topSurfacesShown();
      for (const { group, ov } of overlays.values()) {
        const o = s.objects.find((ob) => ob.id === ov.objectId);
        const inst = o?.instances.find((i) => i.id === ov.instanceId);
        group.visible = !!o && !!inst && overlayFits(ov, o);
        if (group.visible) group.matrix.multiplyMatrices(instanceMatrix(o!, inst!), group.userData.from as THREE.Matrix4);
        group.matrixWorldNeedsUpdate = true;
      }
    };
    const unsubPreview = subscribePatchPreview(() => {
      if (overlays.size !== (getPatchPreview()?.overlays.length ?? 0) || [...overlays.values()].some(({ ov }, i) => ov !== getPatchPreview()?.overlays[i])) buildOverlay();
      else {
        syncOverlay(getState());
        render();
      }
    });
    const unsubSurfaces = subscribeTopSurfaces(() => {
      tint(getState());
      syncView(getState());
      syncOverlay(getState());
      render();
    });
    const tip = document.createElement('div');
    tip.className = 'canvas-tip';
    tip.hidden = true;
    el.appendChild(tip);
    const planarTip = (hit: { object: THREE.Object3D; distance: number; tri?: number } | undefined, at: { clientX: number; clientY: number } | null) => {
      const sf = hit && topSurfacesShown() ? surfaceAt((hit.object.userData as { instance: number }).instance, hit.tri) : null;
      if (sf && at) {
        const r = el.getBoundingClientRect();
        tip.replaceChildren(...surfaceText(sf.inst.surfaces[sf.region]).map((line, i) => {
          const d = document.createElement('div');
          d.textContent = line;
          if (i) d.className = 'canvas-tip-sub';
          return d;
        }));
        tip.hidden = false;
        tip.style.left = `${at.clientX - r.left + 12}px`;
        tip.style.top = `${at.clientY - r.top + 16}px`;
        return;
      }
      const entry = hit && overlayRoot.visible ? overlays.get((hit.object.userData as { instance: number }).instance) : undefined;
      let text = '';
      if (entry?.group.visible && entry.ov.planar.length) {
        const p = ray.ray.at(hit!.distance, new THREE.Vector3()).applyMatrix4(entry.group.matrix.clone().invert());
        const area = entry.ov.planar.find((a) => inFootprint(a.footprint, p.x, p.y));
        text = area ? planarReason(area.reason) : '';
      }
      tip.hidden = !text || !at;
      if (!tip.hidden) {
        const r = el.getBoundingClientRect();
        tip.textContent = text;
        tip.style.left = `${at!.clientX - r.left + 12}px`;
        tip.style.top = `${at!.clientY - r.top + 16}px`;
      }
    };

    let last: State | null = null;
    let lastTower: [unknown, unknown, unknown] = [null, null, null];
    const drawnEdits = (s: State) => viewOptions().overhang ? `${effectiveConfig(s, 'process').support_threshold_angle}` : '';
    const sync = () => {
      const s = getState();
      if (s.wipeTower !== lastTower[0] || s.wipeTowerPos !== lastTower[1] || s.project !== lastTower[2]) {
        lastTower = [s.wipeTower, s.wipeTowerPos, s.project];
        tower.sync(s);
        render();
      }
      if (last && (s.gizmo !== last.gizmo || s.objects !== last.objects || s.selected !== last.selected) && (measureState().first || measureState().hover || last.gizmo === 'measure'))
        setMeasure({});
      if (last && s.slant !== last.slant && s.gizmo === 'slant') {
        syncSlant(s);
        syncGizmo(s);
        render();
        last = { ...last, slant: s.slant };
      }
      if (last && s.cut !== last.cut && s.gizmo === 'cut') {
        syncCut(s);
        syncGizmo(s);
        render();
        last = { ...last, cut: s.cut };
      }
      if (last && s.objects === last.objects && s.selected === last.selected && s.printer === last.printer && s.projectPlates === last.projectPlates &&
          s.plateIndex === last.plateIndex && s.project === last.project && s.job.state === last.job.state &&
          s.filamentSlots === last.filamentSlots && s.slotColours === last.slotColours && s.filamentEdits === last.filamentEdits &&
          s.userPresets === last.userPresets && s.filaments === last.filaments && s.projectMaterials === last.projectMaterials &&
          s.machines === last.machines && s.gizmo === last.gizmo && s.dirty.machine === last.dirty.machine &&
          (s.dirty.process === last.dirty.process || drawnEdits(s) === drawnEdits(last))) return;
      last = s;
      syncBed(s);
      syncGrid(s);
      syncObjects(s);
      if (sectionView().ratio > 0) applySection();
      syncEars();
      paintView.sync(s);
      syncView(s);
      if (!isPaint(s.gizmo)) paintView.showCursor(null);
    };
    (window as unknown as { __plater: unknown }).__plater = { scene, get camera() { return views.camera; }, controls, tc, rotateGizmo, renderer, meshes, grid, frames, hoverMs, hovered: () => hovered, flattenPlanes: () => flattenScene.children, measure: measureState, pick, render, getState, setState };
    const unsub = subscribeStore(sync);
    const unsubLayerCursor = subscribeLayerCursor(() => { tint(getState()); render(); });
    el.appendChild(labelLayer);
    el.appendChild(grid.layer);
    const unsubView = subscribeViewOptions(() => { tint(getState()); syncView(getState()); render(); });
    const unsubSection = subscribeSection(applySection);
    const unsubHoverLink = subscribeHoverLink(() => { if (hoverLink()?.source !== 'canvas') { tint(getState()); render(); } });
    const unsubAssembly = subscribeAssembly(() => { last = null; sync(); render(); });
    const onFit = () => {
      const s = getState(), box = new THREE.Box3();
      if (s.tab !== 'prepare') return;
      for (const o of s.objects) for (const i of o.instances)
        if (!s.selected || isSelected(s.selected, o.id, i.id)) box.union(instanceBox(o, i));
      if (box.isEmpty()) views.view('plate');
      else views.fit(box);
      render();
    };
    window.addEventListener('camera-fit', onFit);
    const unsubTheme = subscribeTheme(() => {
      renderer.setClearColor(sceneColors().background);
      selectionColours();
      overlayColours();
      syncBed(getState());
      render();
    });
    buildOverlay();
    sync();
    resize();
    let unmountMarks: (() => void) | null = null, disposed = false;
    void import('../preflight/marks').then((m) => { if (!disposed) unmountMarks = m.mountMarks(scene, render); });

    return () => {
      clearTimeout(settle);
      disposed = true;
      unmountMarks?.();
      unsub();
      unsubLayerCursor();
      unsubView();
      unsubSection();
      unsubHoverLink();
      unsubAssembly();
      unsubEars();
      window.removeEventListener('camera-fit', onFit);
      labelLayer.remove();
      grid.layer.remove();
      unsubPaintTool();
      unsubTheme();
      unsubPreview();
      unsubSurfaces();
      unwatchRatio();
      window.removeEventListener('pointerup', onTouchUp);
      window.removeEventListener('pointermove', onRightMove);
      window.removeEventListener('pointerup', onRightUp);
      window.removeEventListener('pointercancel', onTouchUp);
      cancelAnimationFrame(hoverFrame);
      ro.disconnect();
      tools.send({ type: 'RELEASE' });
      views.dispose();
      tc.dispose();
      rotateGizmo?.dispose();
      slantRings.dispose();
      unsubMeasure();
      measuring.dispose();

      selectionBounds.dispose();
      for (const edges of outlineCache.values()) edges.dispose();
      document.removeEventListener('visibilitychange', redraw);
      renderer.dispose();
      renderer.forceContextLoss();
      el.removeChild(renderer.domElement);
    };
  }, []);

  useEffect(() => gizmoRef.current(gizmo), [gizmo]);

  return <div ref={host} style={{ position: 'absolute', inset: 0 }} />;
}
