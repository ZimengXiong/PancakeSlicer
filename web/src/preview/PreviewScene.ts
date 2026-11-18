import * as THREE from 'three';
import { CameraViews } from '../plater/CameraViews';
import { getState, isSelected } from '../state/store';
import type { PrinterPreset } from '../state/store';
import { bedKey, buildBed, sceneColors, showBedFor } from '../plater/Bed';
import { subscribeTheme } from '../theme';
import { pixelRatio, watchPixelRatio } from '../plater/pixelRatio';
import { instanceBox, instanceMatrix, parsePoints } from '../plater/geometry';
import type { PlateObject } from '../state/store';
import { colorKey, colorVertices, type ColorContext } from './colors';
import type { PreviewData } from './data';
import { SegmentRenderer, type DrawState, type FrameStats } from './renderer';
import { binOf, type SurfaceGrid } from './surface-grid';
import { toolAxis } from './pose';
import { VOXEL_MM, voxelCentre } from './results';

const SURFACE_LIFT_MM = 0.15;

const FRAME_BUDGET_MS = 14;
const RECOLOR_SLICE_MS = 12;
const RECOLOR_STEP = 32768;
const STILL_MS = 250;

export class PreviewScene {
  readonly renderer: THREE.WebGLRenderer;
  readonly scene = new THREE.Scene();
  readonly views: CameraViews;
  get camera() { return this.views.camera; }
  get controls() { return this.views.controls; }
  private segments: SegmentRenderer;
  private back: { data: PreviewData; segments: SegmentRenderer; uploaded: number; colorCtx: ColorContext | null } | null = null;
  private spare: SegmentRenderer | null = null;
  private removed: { mesh: THREE.InstancedMesh; z: Float32Array } | null = null;
  private overlay = new THREE.Group();
  private hostW = 0;
  private hostH = 0;
  private diffVisible = false;
  private bed: THREE.Group | null = null;
  private bedKey = '';
  private machine: PrinterPreset | undefined;
  private unsubTheme = subscribeTheme(() => {
    this.renderer.setClearColor(sceneColors().background);
    this.shellMat.color.set(sceneColors().shell);
    this.setBed(this.machine);
    this.request();
  });
  private area: THREE.Vector2[] = [];
  private shells = new THREE.Group();
  private shellMat = new THREE.MeshStandardMaterial({ color: sceneColors().shell, transparent: true, opacity: 0.35, depthWrite: false, roughness: 0.8 });
  private tool: THREE.Mesh;
  private surface: { mesh: THREE.Mesh; cells: Uint32Array; grid: SurfaceGrid } | null = null;
  private data: PreviewData | null = null;
  private uploaded = 0;
  private draw: DrawState = { range: [0, 0], roleMask: -1, typeMask: 0, optionMask: 0 };
  private frame = 0;
  private colorCtx: ColorContext | null = null;
  private colorKeyed: string | null = null;
  private recoloring: { next: number; frame: number } | null = null;
  private timer: { ext: any; query: WebGLQuery | null; gpuMs: number } = { ext: null, query: null, gpuMs: NaN };
  lastFrame: FrameStats & { frameMs: number; gpuMs: number; scale: number } = { blocks: 0, culled: 0, lodBlocks: 0, segments: 0, cpuMs: 0, frameMs: 0, gpuMs: NaN, scale: 1 };
  onFrame: (() => void) | null = null;
  onLost: (() => void) | null = null;
  private moving = false;
  private still = 0;
  private upscale: { prog: WebGLProgram; vao: WebGLVertexArrayObject; tex: WebGLTexture; scale: WebGLUniformLocation | null; w: number; h: number } | null = null;
  private scale = 1;
  private basePixelRatio = 1;
  private unwatchRatio = watchPixelRatio(() => this.resize());

  constructor(private host: HTMLElement) {
    this.renderer = new THREE.WebGLRenderer({ antialias: true, preserveDrawingBuffer: true, powerPreference: 'high-performance' });
    this.renderer.setClearColor(sceneColors().background);
    this.renderer.autoClear = true;
    host.appendChild(this.renderer.domElement);
    this.scene.add(new THREE.HemisphereLight(0xffffff, 0x8a8a8a, 1.6));
    const sun = new THREE.DirectionalLight(0xffffff, 1.6);
    sun.position.set(-0.4, -0.6, 1);
    this.scene.add(sun);
    this.scene.add(this.shells);
    this.scene.add(this.overlay);
    this.views = new CameraViews(this.renderer.domElement, () => getState().tab === 'preview', () => this.request());
    this.controls.addEventListener('change', () => this.request());
    window.addEventListener('camera-fit', this.onFit);
    this.controls.addEventListener('start', () => {
      clearTimeout(this.still);
      this.moving = true;
    });
    this.controls.addEventListener('end', () => {
      clearTimeout(this.still);
      this.still = window.setTimeout(() => {
        this.moving = false;
        this.request();
      }, STILL_MS);
    });
    const cone = new THREE.ConeGeometry(1.2, 4, 24);
    cone.rotateX(-Math.PI / 2);
    cone.translate(0, 0, 2);
    this.tool = new THREE.Mesh(cone, new THREE.MeshStandardMaterial({ color: 0xffffff, transparent: true, opacity: 0.6 }));
    this.tool.visible = false;
    this.scene.add(this.tool);
    this.segments = new SegmentRenderer(this.renderer);
    const gl = this.renderer.getContext() as WebGL2RenderingContext;
    this.timer.ext = gl.getExtension('EXT_disjoint_timer_query_webgl2');
    this.renderer.domElement.addEventListener('webglcontextlost', (event) => { event.preventDefault(); this.onLost?.(); });
    new ResizeObserver(() => this.resize()).observe(host);
    this.resize();
  }

  resize() {
    const w = (this.hostW = this.host.clientWidth), h = (this.hostH = this.host.clientHeight);
    if (!w || !h) return;
    const size = this.renderer.getSize(new THREE.Vector2());
    const base = pixelRatio(w, h);
    if (base !== this.basePixelRatio || size.x !== w || size.y !== h) {
      this.basePixelRatio = base;
      this.renderer.setPixelRatio(base);
      this.renderer.setSize(w, h);
    }
    this.views.resize(w, h);
    this.request();
  }

  setBed(machine: PrinterPreset | undefined) {
    this.machine = machine;
    const key = bedKey(machine);
    if (key === this.bedKey) return;
    this.bedKey = key;
    if (this.bed) this.scene.remove(this.bed);
    this.bed = buildBed(machine, () => this.request());
    this.scene.add(this.bed);
    this.area = parsePoints(machine?.config.printable_area);
    this.views.setBed(this.area);
  }

  setShells(objects: PlateObject[], visible: boolean) {
    const bounds = new THREE.Box3();
    for (const o of objects) for (const i of o.instances) bounds.union(instanceBox(o, i));
    this.views.setBounds(bounds);
    this.shells.clear();
    if (visible)
      for (const o of objects)
        for (const inst of o.instances) {
          const m = new THREE.Mesh(o.geometry, this.shellMat);
          m.matrixAutoUpdate = false;
          m.matrix.copy(instanceMatrix(o, inst));
          m.renderOrder = 10;
          this.shells.add(m);
        }
    this.request();
  }

  setData(d: PreviewData) {
    this.data = d;
    this.uploaded = 0;
    this.colorKeyed = null;
    this.segments.reset(d);
    this.request();
  }

  setBackData(d: PreviewData) {
    const segments = this.back?.segments ?? this.spare ?? new SegmentRenderer(this.renderer);
    this.spare = null;
    segments.lodEnabled = this.segments.lodEnabled;
    segments.reset(d);
    this.back = { data: d, segments, uploaded: 0, colorCtx: null };
  }

  syncBack(ctx: ColorContext | null) {
    const b = this.back;
    if (!b || b.data.loaded <= b.uploaded) return;
    const start = b.uploaded, end = b.data.loaded;
    b.segments.addChunk(start, end - start);
    if (ctx) {
      colorVertices(b.data, ctx, b.segments.colors, start, end);
      b.segments.uploadColors(start, end);
    }
    b.colorCtx = ctx;
    b.uploaded = end;
  }

  recolorBack(ctx: ColorContext, from: number, to: number) {
    const b = this.back;
    if (!b) return;
    colorVertices(b.data, ctx, b.segments.colors, from, to);
    b.segments.uploadColors(from, to);
    b.colorCtx = ctx;
  }

  get backData() {
    return this.back?.data ?? null;
  }

  promote() {
    const b = this.back;
    if (!b) return;
    this.spare = this.segments;
    this.segments = b.segments;
    this.data = b.data;
    this.uploaded = b.uploaded;
    this.colorCtx = b.colorCtx;
    this.colorKeyed = null;
    this.back = null;
    this.request();
  }

  dropBack() {
    if (!this.back) return;
    this.spare = this.back.segments;
    this.back = null;
  }

  showDiff(color: [number, number, number] | null) {
    this.draw = { ...this.draw, diffColor: color ?? undefined };
    this.diffVisible = !!color;
    this.request();
  }

  setDiffFlags(flags: Uint8Array | null) {
    this.segments.setDiff(flags);
    this.request();
  }

  setRemoved(keys: Iterable<number> | null, color?: string) {
    if (this.removed) {
      this.overlay.remove(this.removed.mesh);
      this.removed.mesh.geometry.dispose();
      (this.removed.mesh.material as THREE.Material).dispose();
      this.removed.mesh.dispose();
      this.removed = null;
    }
    const centres = keys ? [...keys].map(voxelCentre).sort((a, b) => a[2] - b[2]) : [];
    if (centres.length) {
      const box = new THREE.BoxGeometry(VOXEL_MM * 0.9, VOXEL_MM * 0.9, VOXEL_MM * 0.9);
      const mat = new THREE.MeshBasicMaterial({ color: color ?? '#e07000', transparent: true, opacity: 0.45, depthWrite: false });
      const mesh = new THREE.InstancedMesh(box, mat, centres.length);
      const m = new THREE.Matrix4();
      const z = new Float32Array(centres.length);
      centres.forEach((c, i) => {
        mesh.setMatrixAt(i, m.makeTranslation(c[0], c[1], c[2]));
        z[i] = c[2];
      });
      mesh.instanceMatrix.needsUpdate = true;
      mesh.frustumCulled = false;
      mesh.renderOrder = 11;
      this.overlay.add(mesh);
      this.removed = { mesh, z };
    }
    this.request();
  }

  setRemovedTop(top: number) {
    const r = this.removed;
    if (!r) return;
    let lo = 0, hi = r.z.length;
    while (lo < hi) {
      const mid = (lo + hi) >> 1;
      if (r.z[mid] - VOXEL_MM / 2 <= top) lo = mid + 1;
      else hi = mid;
    }
    if (r.mesh.count !== lo) {
      r.mesh.count = lo;
      this.request();
    }
  }

  clear() {
    this.dropBack();
    this.data = null;
    this.uploaded = 0;
    this.tool.visible = false;
    this.request();
  }

  holds(d: PreviewData) {
    return this.data === d;
  }

  sync() {
    const d = this.data;
    if (!d || d.loaded <= this.uploaded) return;
    const start = this.uploaded;
    this.segments.addChunk(start, d.loaded - start);
    if (this.colorCtx) {
      colorVertices(d, this.colorCtx, this.segments.colors, start, d.loaded);
      this.segments.uploadColors(start, d.loaded);
    }
    this.uploaded = d.loaded;
    this.request();
  }

  setColors(ctx: ColorContext) {
    const key = colorKey(ctx);
    const same = key !== null && key === this.colorKeyed;
    this.colorCtx = ctx;
    this.colorKeyed = key;
    const d = this.data;
    if (!d || same) return;
    if (this.recoloring) cancelAnimationFrame(this.recoloring.frame);
    const segments = this.segments, job = (this.recoloring = { next: 0, frame: 0 });
    const slice = () => {
      if (this.recoloring !== job || this.data !== d || this.segments !== segments) return;
      const t0 = performance.now(), from = job.next, end = d.loaded;
      let i = from;
      while (i < end && performance.now() - t0 < RECOLOR_SLICE_MS) {
        const to = Math.min(end, i + RECOLOR_STEP);
        colorVertices(d, ctx, segments.colors, i, to);
        i = to;
      }
      segments.uploadColors(from, i);
      job.next = i;
      this.request();
      if (i < end) job.frame = requestAnimationFrame(slice);
      else this.recoloring = null;
    };
    slice();
  }

  setDrawState(s: DrawState, toolAt: number | null) {
    this.draw = { ...s, focusMask: this.draw.focusMask, diffColor: this.draw.diffColor };
    const d = this.data;
    if (d && toolAt != null && toolAt < d.loaded) {
      this.setToolPose(toolAt);
    } else this.tool.visible = false;
    this.request();
  }

  setToolPose(i: number, dir?: [number, number, number]) {
    const d = this.data;
    if (!d || i < 0 || i >= d.loaded) { this.tool.visible = false; return; }
    this.tool.position.fromArray(d.pos, i * 4);
    const axis = new THREE.Vector3(...(dir ?? toolAxis(d, i)));
    if (axis.lengthSq() < 1e-12) axis.set(0, 0, 1);
    this.tool.quaternion.setFromUnitVectors(new THREE.Vector3(0, 0, 1), axis.normalize());
    this.tool.visible = true;
    this.request();
  }

  pick(clientX: number, clientY: number): number | null {
    const rect = this.renderer.domElement.getBoundingClientRect();
    if (!rect.width || !rect.height) return null;
    const ray = new THREE.Raycaster();
    ray.setFromCamera(new THREE.Vector2(2 * (clientX - rect.left) / rect.width - 1, 1 - 2 * (clientY - rect.top) / rect.height), this.camera);
    const pixel = this.camera instanceof THREE.PerspectiveCamera
      ? 2 * Math.tan(this.camera.fov * Math.PI / 360) * this.camera.position.distanceTo(this.controls.target) / rect.height
      : (this.camera.top - this.camera.bottom) / this.camera.zoom / rect.height;
    return this.segments.pick(ray.ray, this.draw, 6 * pixel);
  }

  setFocus(mask: number | null) {
    if ((this.draw.focusMask ?? -1) === (mask ?? -1)) return;
    this.draw = { ...this.draw, focusMask: mask ?? -1 };
    this.request();
  }

  copyCamera(from: PreviewScene) {
    this.views.copy(from.views);
    this.views.resize(Math.max(1, this.host.clientWidth), Math.max(1, this.host.clientHeight));
    this.request();
  }

  private onFit = () => {
    const s = getState(), box = new THREE.Box3();
    if (s.tab !== 'preview') return;
    for (const o of s.objects) for (const i of o.instances)
      if (!s.selected || isSelected(s.selected, o.id, i.id)) box.union(instanceBox(o, i));
    if (box.isEmpty() && this.data) box.set(new THREE.Vector3(...this.data.bbox.min), new THREE.Vector3(...this.data.bbox.max));
    if (box.isEmpty()) this.views.view('plate');
    else this.fit(box.min, box.max);
    this.request();
  };

  fit(min: THREE.Vector3, max: THREE.Vector3) {
    this.views.fit(new THREE.Box3(min, max));
    this.request();
  }

  setSurface(grid: SurfaceGrid | null) {
    if (this.surface?.grid === grid) return;
    if (this.surface) {
      this.scene.remove(this.surface.mesh);
      this.surface.mesh.geometry.dispose();
      (this.surface.mesh.material as THREE.Material).dispose();
      this.surface = null;
    }
    if (grid) {
      const { nx, ny, pitch, origin, rms, meshZ, eligible } = grid;
      const cells: number[] = [];
      for (let i = 0; i < nx * ny; ++i) if (eligible[i] > 0 && Number.isFinite(rms[i]) && Number.isFinite(meshZ[i])) cells.push(i);
      const pos = new Float32Array(cells.length * 18), col = new Float32Array(cells.length * 18);
      cells.forEach((i, k) => {
        const x0 = origin[0] + (i % nx) * pitch, y0 = origin[1] + Math.floor(i / nx) * pitch, z = meshZ[i] + SURFACE_LIFT_MM;
        const [r, g, b] = binOf(rms[i] * 1000).color;
        const c = new THREE.Color().setRGB(r / 255, g / 255, b / 255, THREE.SRGBColorSpace);
        const corners = [[x0, y0], [x0 + pitch, y0], [x0 + pitch, y0 + pitch], [x0, y0], [x0 + pitch, y0 + pitch], [x0, y0 + pitch]];
        corners.forEach(([x, y], v) => {
          pos.set([x, y, z], k * 18 + v * 3);
          col.set([c.r, c.g, c.b], k * 18 + v * 3);
        });
      });
      const geometry = new THREE.BufferGeometry();
      geometry.setAttribute('position', new THREE.BufferAttribute(pos, 3));
      geometry.setAttribute('color', new THREE.BufferAttribute(col, 3));
      const material = new THREE.MeshBasicMaterial({ vertexColors: true, side: THREE.DoubleSide });
      const mesh = new THREE.Mesh(geometry, material);
      this.scene.add(mesh);
      this.surface = { mesh, cells: Uint32Array.from(cells), grid };
    }
    this.request();
  }

  pickSurface(px: number, py: number): number {
    const s = this.surface;
    if (!s) return -1;
    const ray = new THREE.Raycaster();
    ray.setFromCamera(new THREE.Vector2((px / this.host.clientWidth) * 2 - 1, -(py / this.host.clientHeight) * 2 + 1), this.camera);
    const hit = ray.intersectObject(s.mesh)[0];
    return hit?.faceIndex != null ? s.cells[hit.faceIndex >> 1] : -1;
  }

  get segmentRenderer() {
    return this.segments;
  }

  set lod(on: boolean) {
    this.segments.lodEnabled = on;
    this.request();
  }

  gpuBytes() {
    return this.segments.gpuBytes();
  }

  request() {
    if (this.frame || !this.hostW) return;
    this.frame = requestAnimationFrame(() => {
      this.frame = 0;
      if (this.hostW) this.renderNow();
    });
  }

  renderNow() {
    const t0 = performance.now();
    const gl = this.renderer.getContext() as WebGL2RenderingContext;
    const tq = this.timer;
    if (tq.ext && tq.query && gl.getQueryParameter(tq.query, gl.QUERY_RESULT_AVAILABLE)) {
      if (!gl.getParameter(tq.ext.GPU_DISJOINT_EXT)) tq.gpuMs = gl.getQueryParameter(tq.query, gl.QUERY_RESULT) / 1e6;
      gl.deleteQuery(tq.query);
      tq.query = null;
    }
    const cost = Number.isFinite(tq.gpuMs) ? tq.gpuMs : this.lastFrame.frameMs;
    const scale = (this.scale = !this.moving ? 1 : cost > FRAME_BUDGET_MS * 1.5 ? 0.5 : this.scale);
    const w = this.hostW, h = this.hostH;
    if (scale < 1) this.renderer.setViewport(0, 0, w * scale, h * scale);
    const timing = tq.ext && !tq.query;
    if (timing) {
      tq.query = gl.createQuery();
      gl.beginQuery(tq.ext.TIME_ELAPSED_EXT, tq.query!);
    }
    this.shells.visible = false;
    this.overlay.visible = false;
    showBedFor(this.bed, this.camera);
    this.renderer.render(this.scene, this.camera);
    if (this.data) this.segments.render(this.camera, this.draw, Math.round(this.renderer.domElement.height * scale));
    const removed = !!this.removed && this.diffVisible;
    if (this.shells.children.length || removed) {
      this.shells.visible = true;
      this.overlay.visible = removed;
      const bedVisible = this.bed?.visible;
      if (this.bed) this.bed.visible = false;
      const tv = this.tool.visible;
      this.tool.visible = false;
      this.renderer.autoClear = false;
      this.renderer.render(this.scene, this.camera);
      this.renderer.autoClear = true;
      if (this.bed) this.bed.visible = bedVisible!;
      this.tool.visible = tv;
    }
    if (scale < 1) {
      this.stretch(gl, scale);
      this.renderer.setViewport(0, 0, w, h);
    }
    if (timing) gl.endQuery(tq.ext.TIME_ELAPSED_EXT);
    this.lastFrame = { ...this.segments.stats, frameMs: performance.now() - t0, gpuMs: tq.gpuMs, scale: this.scale };
    this.onFrame?.();
  }

  private stretch(gl: WebGL2RenderingContext, scale: number) {
    const W = gl.drawingBufferWidth, H = gl.drawingBufferHeight;
    const vw = Math.round(W * scale), vh = Math.round(H * scale);
    let u = this.upscale;
    if (!u) {
      const shader = (type: number, src: string) => {
        const sh = gl.createShader(type)!;
        gl.shaderSource(sh, src);
        gl.compileShader(sh);
        return sh;
      };
      const prog = gl.createProgram()!;
      gl.attachShader(prog, shader(gl.VERTEX_SHADER, `#version 300 es
out vec2 uv;
void main() { uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2); gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0); }`));
      gl.attachShader(prog, shader(gl.FRAGMENT_SHADER, `#version 300 es
precision mediump float;
uniform sampler2D tex;
uniform vec2 scale;
in vec2 uv;
out vec4 frag;
void main() { frag = texture(tex, uv * scale); }`));
      gl.linkProgram(prog);
      u = this.upscale = { prog, vao: gl.createVertexArray()!, tex: gl.createTexture()!, scale: gl.getUniformLocation(prog, 'scale'), w: 0, h: 0 };
    }
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, u.tex);
    if (u.w !== W || u.h !== H) {
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, W, H, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
      u.w = W;
      u.h = H;
    }
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.copyTexSubImage2D(gl.TEXTURE_2D, 0, 0, 0, 0, 0, vw, vh);
    gl.viewport(0, 0, W, H);
    gl.disable(gl.DEPTH_TEST);
    gl.disable(gl.BLEND);
    gl.disable(gl.SCISSOR_TEST);
    gl.useProgram(u.prog);
    gl.bindVertexArray(u.vao);
    gl.uniform2f(u.scale, (vw - 0.5) / W, (vh - 0.5) / H);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    gl.bindVertexArray(null);
    this.renderer.resetState();
  }

  bench(frames = 120, zoom = 1): Promise<{ scale: number; medianIntervalMs: number; p95IntervalMs: number; medianFrameMs: number; p95FrameMs: number; medianGpuMs: number; segments: number; lodBlocks: number; blocks: number }> {
    const c = this.controls;
    const target = c.target.clone();
    const offset = this.camera.position.clone().sub(target).multiplyScalar(zoom);
    const intervals: number[] = [], cpu: number[] = [], gpu: number[] = [];
    let last = 0, i = 0;
    this.moving = true;
    return new Promise((resolve) => {
      const step = (t: number) => {
        if (last) intervals.push(t - last);
        last = t;
        const a = (i / frames) * Math.PI * 2;
        const o = offset.clone().applyAxisAngle(new THREE.Vector3(0, 0, 1), a);
        this.camera.position.copy(target).add(o);
        this.camera.lookAt(target);
        const f0 = performance.now();
        this.renderNow();
        (this.renderer.getContext() as WebGL2RenderingContext).finish();
        cpu.push(performance.now() - f0);
        if (Number.isFinite(this.lastFrame.gpuMs)) gpu.push(this.lastFrame.gpuMs);
        if (++i <= frames) requestAnimationFrame(step);
        else {
          const med = (v: number[]) => (v.length ? [...v].sort((x, y) => x - y)[Math.floor(v.length / 2)] : NaN);
          const p95 = (v: number[]) => (v.length ? [...v].sort((x, y) => x - y)[Math.floor(v.length * 0.95)] : NaN);
          this.moving = false;
          this.request();
          c.update();
          resolve({ scale: this.lastFrame.scale, medianIntervalMs: med(intervals), p95IntervalMs: p95(intervals), medianFrameMs: med(cpu), p95FrameMs: p95(cpu), medianGpuMs: med(gpu),
            segments: this.lastFrame.segments, lodBlocks: this.lastFrame.lodBlocks, blocks: this.lastFrame.blocks });
        }
      };
      requestAnimationFrame(step);
    });
  }

  dispose() {
    clearTimeout(this.still);
    window.removeEventListener('camera-fit', this.onFit);
    if (this.upscale) {
      const gl = this.renderer.getContext() as WebGL2RenderingContext;
      gl.deleteProgram(this.upscale.prog);
      gl.deleteVertexArray(this.upscale.vao);
      gl.deleteTexture(this.upscale.tex);
    }
    this.unsubTheme();
    this.unwatchRatio();
    cancelAnimationFrame(this.frame);
    this.segments.dispose();
    this.back?.segments.dispose();
    this.spare?.dispose();
    this.setRemoved(null);
    this.views.dispose();
    this.renderer.dispose();
    this.host.removeChild(this.renderer.domElement);
  }
}
