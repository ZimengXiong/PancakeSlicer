import { OrderBar } from './OrderBar';
import { phoneSheetQuery } from '../workspace-sheet';
import { Skeleton } from './Skeleton';
import { loadSource, noteViewerProgress, viewerFirstMoves, viewerReady, viewerError, scrubViewer } from '../flow/viewer';
import { Suspense, useEffect, useMemo, useReducer, useRef, useState } from 'react';
import { getState, printerPreset, setState, subscribeStore, useStore } from '../state/store';
import { layerOfMove, peekJump, setGcodeListener, setJumpListener, setLoadedStream, takeGcode, takeJump } from '../preview/jump';
import { setCompareWanted, useCompareWanted, useStandardData } from '../compare';
import { wholePartLayers } from '../preview/explain-record';
import { useMethods } from '../methods';
import { computeRanges, drawMasks, mergeRanges, EXTENDED_VIEWS, hexToRgb, type ColorContext, type NonplanarColor, type Ranges, type ViewType, type Visibility } from '../preview/colors';
import { loadMoves, type PreviewData } from '../preview/data';
import { NonplanarHover, explain, type HoverInfo } from '../preview/hover';
import { PreviewScene } from '../preview/PreviewScene';
import { loadSurfaceGrid, useSurfaceGrid } from '../preview/surface-grid';
import { clearanceScale, loadClearanceScale } from '../preview/clearance';
import { Legend } from './PreviewLegend';
import { LayerSlider } from './PreviewSliders';
import { EventList, EventMenu, eventTicks } from './LayerEvents';
import { ownsKeys } from '../keys';
import { numberDialog, openFileDialog, slice } from '../actions';
import { toggleViewOption, useViewOptions } from '../plater/viewOptions';
import { filamentColor } from '../plater/filamentColor';
import { setVisibleToolpaths } from '../preview/visible';
import { preloadable } from '../preloadable';
import './preview.css';
import { slotFilaments } from '../profiles/slots';

const SHOW_STATS = new URLSearchParams(location.search).has('stats');
const ExplainLegend = preloadable(() => import('./PreviewExplain').then((m) => ({ default: m.ExplainLegend })));
const CompareView = preloadable(() => import('./PreviewExplain').then((m) => ({ default: m.CompareView })));
const ViewSwitches = preloadable(() => import('./PreviewExplain').then((m) => ({ default: m.ViewSwitches })));
const SliceDiff = preloadable(() => import('./SliceDiff').then((m) => ({ default: m.SliceDiff })));
const SliceDiffSummary = preloadable(() => import('./SliceDiff').then((m) => ({ default: m.SliceDiffSummary })));
const DoneComparing = preloadable(() => import('./SliceDiff').then((m) => ({ default: m.DoneComparing })));
const MethodsCompare = preloadable(() => import('./MethodsCompare').then((m) => ({ default: m.MethodsCompare })));
const DoneMethods = preloadable(() => import('./MethodsCompare').then((m) => ({ default: m.DoneMethods })));
const setStandardCompare = (on: boolean) => setCompareWanted(on ? 'standard' : null);
const GcodeWindow = preloadable(() => import('./GcodeWindow'));
const ToolPosition = preloadable(() => import('./ToolPosition'));

export const jobColours = () => slotFilaments(getState()).map((_, i) => filamentColor(getState(), i + 1));

export function colorContext(d: PreviewData, ui: Pick<PreviewUi, 'view' | 'nonplanar'>, ranges: Ranges): ColorContext {
  const extruderColors = (d.meta.extruder_colors?.length ? d.meta.extruder_colors : jobColours()).map(hexToRgb);
  return { view: ui.view, nonplanar: ui.nonplanar, ranges, extruderColors, colorPrintColors: (d.meta.color_print_colors ?? []).map(hexToRgb), clearance: clearanceScale() };
}

let current: { key: string; data: PreviewData; cancel(): void } | null = null;
let live: { header(d: PreviewData): void; chunk(d: PreviewData): void; done(d: PreviewData): void; error(d: PreviewData): void } | null = null;

const extendedJobs = new Set<string>();
let extendedView = false;
export const canExtend = (d: PreviewData) => !!d.sourceUrl && d.sourceUrl === getState().api?.movesUrl(getState().job.id ?? '');

function startLoad(src: { key: string; url?: string; file?: File; shellLayers?: boolean }) {
  current?.cancel();
  loadSource({ id: src.key });
  const mine = (d: PreviewData) => current?.data === d;
  const note = (d: PreviewData) => noteViewerProgress({ id: src.key, loaded: d.loaded, total: d.n, bytes: d.bytes });
  const handle = loadMoves(src, {
    header: (d) => { if (!mine(d)) return; note(d); live?.header(d); },
    chunk: (d) => { if (!mine(d)) return; note(d); live?.chunk(d); viewerFirstMoves(src.key, d.loaded); },
    done: (d) => mine(d) && (live ? live.done(d) : viewerReady(src.key, d.loaded)),
    error: (d) => mine(d) && (live ? live.error(d) : viewerError(src.key, d.error ?? 'Oops! Couldn\'t load the 3D preview')),
  }, src.key.startsWith('job:') ? jobColours() : undefined);
  current = { key: src.key, ...handle };
  setLoadedStream({ key: src.key, data: handle.data });
}

subscribeStore(() => {
  const { job } = getState();
  if (live || job.state !== 'done' || !job.id || current?.key.startsWith(`job:${job.id}`)) return;
  const src = source();
  if (src?.key.startsWith('job:')) startLoad(src);
});

function source(): { key: string; url?: string; file?: File; shellLayers?: boolean } | null {
  const q = new URLSearchParams(location.search).get('moves');
  if (q) return { key: `url:${q}`, url: q };
  const s = getState();
  if (s.job.state !== 'done' || !s.job.id || !s.api) return null;
  const ext = extendedJobs.has(s.job.id) || (extendedView);
  return { key: `job:${s.job.id}${ext ? ':extended' : ''}`, url: s.api.movesUrl(s.job.id) + (ext ? '?extended=1' : ''), shellLayers: wholePartLayers(s.job.status?.record) };
}

const layerShare = (d: PreviewData, k: number) => {
  const L = d.layers, last = L.length - 1;
  if (k <= 0 || last <= 0) return 0;
  if (k >= last) return 1;
  const z0 = L[0].z, z1 = L[last].z;
  return z1 > z0 ? Math.min(1, Math.max(0, (L[k].z - z0) / (z1 - z0))) : k / last;
};

export function sharedRanges(d: PreviewData, other: PreviewData | null, ui: Pick<PreviewUi, 'view' | 'nonplanar' | 'vis'>): Ranges {
  const slope = ui.view === 'nonplanar' && ui.nonplanar === 'slope';
  if (slope) d.computeSlope();
  if (!other?.header) return computeRanges(d, ui.vis);
  if (slope) other.computeSlope();
  return mergeRanges(computeRanges(d, ui.vis), computeRanges(other, ui.vis));
}

export interface PreviewUi {
  view: ViewType;
  nonplanar: NonplanarColor;
  vis: Visibility;
  shells: boolean;
  surface: boolean;
}

export function PreviewView() {
  const job = useStore((s) => s.job);
  const objects = useStore((s) => s.objects);
  const host = useRef<HTMLDivElement>(null);
  const sceneRef = useRef<PreviewScene | null>(null);
  const [, bump] = useReducer((x: number) => x + 1, 0);
  const { gcodeWindow } = useViewOptions();
  const [dropped, setDropped] = useState<File | null>(null);
  const [extendTick, setExtendTick] = useState(0);
  const [data, setData] = useState<PreviewData | null>(current?.data ?? null);
  const [ui, setUi] = useState<PreviewUi>({
    view: 'feature',
    nonplanar: 'shell',
    vis: { travel: false, wipe: false, retract: false, unretract: false, seam: true, hiddenRoles: new Set() },
    shells: false,
    surface: false,
  });
  const ownStatus = data?.sourceUrl && job.id && data.sourceUrl === getState().api?.movesUrl(job.id) ? job.status : undefined;
  const grid = useSurfaceGrid(ownStatus, ui.surface);
  useEffect(() => {
    if (data?.done && ownStatus?.has_surface_grid) loadSurfaceGrid(ownStatus.id).catch(() => {});
  }, [data?.done, ownStatus?.id, ownStatus?.has_surface_grid]);
  const [layers, setLayers] = useState<[number, number]>([0, Number.MAX_SAFE_INTEGER]);
  const [movePos, setMovePos] = useState<number | null>(null);
  const ranges = useRef<Ranges | null>(null);
  const hover = useRef(new NonplanarHover());
  const drawRange = useRef<[number, number]>([1, 1]);
  const [tip, setTip] = useState<(HoverInfo & { x: number; y: number }) | null>(null);
  const tapAt = useRef<{ x: number; y: number } | null>(null);
  const showTip = (e: React.PointerEvent, busy: boolean) => {
    const scene = sceneRef.current;
    const r = host.current!.getBoundingClientRect();
    const x = e.clientX - r.left, y = e.clientY - r.top;
    const cells = !busy && scene ? grid : null;
    if (cells) {
      const cell = scene!.pickSurface(x, y);
      return setTip(cell < 0 ? null : { title: `Top error ${Math.round(cells.rms[cell] * 1000)} µm`, body: '', x, y });
    }
    if (ui.view !== 'nonplanar' || busy || !scene || !data?.states) return tip && setTip(null);
    const id = hover.current.pick(data, scene.camera, r.width, r.height, drawRange.current, ui.vis.hiddenRoles, x, y);
    setTip(id < 0 ? null : { ...explain(data, id, job.status?.record), x, y });
  };
  const lastRanges = useRef(0);
  const loadBump = useRef<{ pending: boolean } | null>(null);
  const bumpLoad = () => {
    if (loadBump.current) return void (loadBump.current.pending = true);
    bump();
    const slot = { pending: false };
    loadBump.current = slot;
    setTimeout(() => {
      loadBump.current = null;
      if (slot.pending) bump();
    }, 100);
  };
  const events = useStore((s) => s.events);
  const [eventMenu, setEventMenu] = useState<{ anchor: DOMRect; layer: number } | null>(null);
  const [oneLayer, setOneLayer] = useState(false);
  const oneBackup = useRef<number | null>(null);
  const methods = !!useMethods();
  const wantedCompare = useCompareWanted();
  const compare = methods ? null : wantedCompare;
  const setCompare = setStandardCompare;

  const [epoch, setEpoch] = useState(0);
  const [restoring, setRestoring] = useState(false);
  const lostCount = useRef(0);
  useEffect(() => {
    const scene = new PreviewScene(host.current!);
    sceneRef.current = scene;
    let retry = 0;
    scene.onLost = () => {
      setRestoring(true);
      retry = window.setTimeout(() => setEpoch((n) => n + 1), Math.min(400 * 2 ** lostCount.current++, 6000));
    };
    const settled = window.setTimeout(() => { lostCount.current = 0; setRestoring(false); }, 1500);
    let lastBump = 0;
    if (SHOW_STATS) scene.onFrame = () => {
      if (performance.now() - lastBump > 500) {
        lastBump = performance.now();
        bump();
      }
    };
    return () => {
      clearTimeout(retry);
      clearTimeout(settled);
      scene.onLost = null;
      scene.dispose();
      sceneRef.current = null;
    };
  }, [epoch]);

  const machine = useStore(printerPreset);
  useEffect(() => sceneRef.current?.setBed(machine), [machine, epoch]);
  useEffect(() => sceneRef.current?.setShells(objects, ui.shells), [objects, ui.shells, epoch]);

  const colorCtx = (d: PreviewData) => colorContext(d, ui, ranges.current!);
  const other = useStandardData();
  const comparingRef = useRef(false);
  const recolor = (d: PreviewData) => {
    ranges.current = sharedRanges(d, comparingRef.current ? other : null, ui);
    lastRanges.current = performance.now();
    sceneRef.current?.setColors(colorCtx(d));
  };

  const handlers: NonNullable<typeof live> = {
    header: (d) => {
      sceneRef.current?.setData(d);
      setData(d);
      ranges.current = computeRanges(d, ui.vis);
      bump();
    },
    chunk: (d) => {
      if (!ranges.current || performance.now() - lastRanges.current > 500) recolor(d);
      sceneRef.current?.sync();
      bumpLoad();
    },
    done: (d) => {
      recolor(d);
      if (current) viewerReady(current.key, d.loaded);
      bump();
    },
    error: (d) => {
      if (current) viewerError(current.key, d.error ?? 'Oops! Couldn\'t load the 3D preview');
      setData(d);
      bump();
    },
  };
  live = handlers;
  useEffect(() => {
    live = handlers;
    return () => {
      if (live === handlers) live = null;
    };
  });
  useEffect(() => setDropped(null), [job.id]);
  useEffect(() => {
    const src = dropped ? { key: `file:${dropped.name}:${dropped.size}`, file: dropped } : source();
    if (!src) {
      loadSource(null);
      if (current) {
        current.cancel();
        current = null;
        setLoadedStream(null);
        sceneRef.current?.clear();
        setData(null);
        setMovePos(null);
      }
      return;
    }
    if (current?.key === src.key) {
      loadSource({id: src.key});
      if (current.data.done) viewerReady(src.key, current.data.loaded);
      else if (current.data.error) viewerError(src.key, current.data.error);
      const d = current.data;
      const scene = sceneRef.current;
      if (d.header && scene && !scene.holds(d)) {
        scene.setData(d);
        recolor(d);
        scene.sync();
      }
      return;
    }
    startLoad(src);
    setData(null);
    setLayers([0, Number.MAX_SAFE_INTEGER]);
    setMovePos(null);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [job.id, job.state, dropped, extendTick]);

  useEffect(() => {
    if (!data?.header) return;
    recolor(data);
    bump();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [ui.view, ui.nonplanar, ui.vis]);

  const layerCount = data?.layers.length ?? 0;
  const hi = Math.min(layers[1], Math.max(0, layerCount - 1));
  const lo = oneLayer ? hi : Math.min(layers[0], Math.max(0, layerCount - 1));
  const [topFirst, topEnd] = data && layerCount ? data.layerSpan(hi, hi) : [1, 1];
  const topMoves = Math.max(0, topEnd - topFirst);
  const pos = movePos == null ? topMoves : Math.min(movePos, topMoves);
  const seekOrder = (vertex: number) => {
    if (!data?.layers.length) return;
    const move = Math.max(1, Math.min(data.loaded - 1, vertex));
    const layer = layerOfMove(data, move);
    if (layer === null) return;
    setLayers(([first]) => [Math.min(first, layer), layer]);
    setMovePos(move - data.layers[layer].first + 1);
  };
  useEffect(() => {
    const f = takeGcode();
    if (f) setDropped(f);
    setGcodeListener((file) => setDropped(file));
    return () => setGcodeListener(null);
  }, []);
  const [jumpTick, setJumpTick] = useState(0);
  useEffect(() => {
    setJumpListener(() => setJumpTick((n) => n + 1));
    return () => setJumpListener(null);
  }, []);
  useEffect(() => {
    const move = peekJump();
    if (move == null || !data?.layers.length) return;
    const k = layerOfMove(data, move);
    if (k === null) return;
    takeJump();
    setLayers([0, k]);
    setMovePos(move - data.layers[k].first + 1);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [data, data?.loaded, data?.done, jumpTick]);
  useEffect(() => {
    const scene = sceneRef.current;
    if (epoch === 0 || !scene || !data?.header) return;
    scene.setData(data);
    recolor(data);
    scene.sync();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [epoch]);
  useEffect(() => {
    const scene = sceneRef.current;
    if (!scene || !data) return;
    const [s0] = layerCount ? data.layerSpan(lo, hi) : [1];
    const s1 = topFirst + pos;
    drawRange.current = [s0, s1];
    scene.setDrawState({ range: [s0, s1], ...drawMasks(ui.vis, !!grid) }, pos < topMoves ? s1 - 1 : null);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [data, data?.loaded, data?.done, lo, hi, pos, topFirst, topMoves, ui.vis, ui.surface, layerCount, job.status, grid, epoch]);

  (window as unknown as { __preview: unknown }).__preview = {
    scene: sceneRef.current,
    data: current?.data,
    setLayers: (a: number, b: number) => { setLayers([a, b]); setMovePos(null); },
    setMove: (p: number | null) => setMovePos(p),
    setView: (view: ViewType, nonplanar?: NonplanarColor) => setUi((u) => ({ ...u, view, nonplanar: nonplanar ?? u.nonplanar })),
    setVis: (v: Partial<Visibility>, shells?: boolean) => setUi((u) => ({ ...u, vis: { ...u.vis, ...v }, shells: shells ?? u.shells })),
  };

  useEffect(() => { setVisibleToolpaths(data?.done ? { data, layers } : null); }, [data, data?.done, layers]);
  useEffect(() => () => setVisibleToolpaths(null), []);
  useEffect(() => { oneBackup.current = null; }, [layerCount]);
  const toggleOneLayer = () => {
    if (!layerCount) return;
    const top = layerCount - 1, cur = Math.min(layers[1], top);
    if (oneLayer) {
      oneBackup.current = cur;
      setLayers([0, Number.MAX_SAFE_INTEGER]);
    } else {
      const back = oneBackup.current === null || oneBackup.current > top ? Math.floor(top / 2) : oneBackup.current;
      setLayers([0, cur === top ? back : cur]);
    }
    setOneLayer(!oneLayer);
    setMovePos(null);
  };
  const jumpToLayer = () => {
    if (!layerCount) return;
    void numberDialog('Jump to layer', `Jump to layer (1–${layerCount}):`, hi + 1, 1, layerCount).then((n) => {
      if (n === null) return;
      setLayers(([a]) => [Math.min(a, n - 1), n - 1]);
      setMovePos(null);
    });
  };

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (ownsKeys(e) || e.ctrlKey || e.metaKey || e.altKey || getState().tab !== 'preview') return;
      if (['ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(e.key)) scrubViewer(topFirst + pos - 1, hi);
      if (e.key === 'ArrowUp' || e.key === 'ArrowDown') {
        const d = e.key === 'ArrowUp' ? 1 : -1;
        setLayers(([a, b]) => [a, Math.max(a, Math.min(layerCount - 1, Math.min(b, layerCount - 1) + d))]);
        setMovePos(null);
      } else if (e.key === 'ArrowLeft' || e.key === 'ArrowRight') {
        const d = e.key === 'ArrowRight' ? 1 : -1;
        setMovePos((p) => Math.max(1, Math.min(topMoves, (p ?? topMoves) + d)));
      } else if (e.key === 'c' || e.key === 'C') toggleViewOption('gcodeWindow');
      else if (e.key === 'l' || e.key === 'L') toggleOneLayer();
      else if (e.key === 'G' && e.shiftKey) jumpToLayer();
      else if (e.key === 'Home') setMovePos(1);
      else if (e.key === 'End') setMovePos(null);
      else return;
      e.preventDefault();
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [layerCount, topMoves, oneLayer, layers, hi]);

  const record = ownStatus?.record;
  const comparing = !!compare && !!record && !job.stale;
  comparingRef.current = comparing;
  useEffect(() => {
    if (!data?.header) return;
    recolor(data);
    bump();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [other, other?.done, comparing]);
  const hasGrid = !!ownStatus?.has_surface_grid;
  const before = comparing && compare === 'previous';
  const explainKey = useMemo(() => methods ? <Suspense fallback={null}><MethodsCompare /></Suspense> : data && record && (
    <Suspense fallback={null}>
      {comparing && compare === 'previous' ? <><SliceDiff data={data} record={record} /><div className="np-toggles"><ViewSwitches ui={ui} setUi={setUi} hasSurfaceGrid={hasGrid} /></div></>
        : <ExplainLegend data={data} record={record} hasSurfaceGrid={hasGrid} ui={ui} setUi={setUi} compare={comparing && compare === 'standard'} setCompare={setCompare} />}
    </Suspense>
  ), [data, data?.done, record, hasGrid, ui, comparing, compare, setCompare, methods]);
  useEffect(() => sceneRef.current?.setSurface(grid), [grid, epoch]);
  useEffect(() => {
    void loadClearanceScale(record).then((changed) => { if (changed && data?.header) { recolor(data); bump(); } });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [record]);
  useEffect(() => {
    extendedView = EXTENDED_VIEWS.has(ui.view);
    if (!data?.header || data.header.extended || !EXTENDED_VIEWS.has(ui.view)) return;
    const id = getState().job.id;
    if (!dropped && id && canExtend(data)) {
      extendedJobs.add(id);
      setExtendTick((t) => t + 1);
    } else setUi((u) => ({ ...u, view: 'feature' }));
  }, [data, ui.view]);
  const status = job.state === 'running' || job.state === 'submitting'
    ? null
    : !data && !source() && !dropped
    ? job.state === 'done' || methods ? null : objects.length ? 'Not sliced yet' : 'Plate is empty'
    : data?.error
      ? /^404 /.test(data.error)
        ? 'The slicing service didn\'t generate 3D preview data for this job, but you can still export your sliced file!'
        : `Oops! Couldn't load toolpaths: ${data.error}`
      : null;

  return (
    <div
      className={comparing ? 'preview compare' : 'preview'}
      onDragOver={(e) => e.preventDefault()}
      onDrop={(e) => {
        const f = e.dataTransfer.files[0];
        if (f && /\.(moves|bin|gz)$/i.test(f.name)) {
          e.preventDefault();
          e.stopPropagation();
          setDropped(f);
        }
      }}
    >
      <div
        ref={host}
        className="preview-canvas"
        onPointerLeave={(e) => e.pointerType === 'mouse' && setTip(null)}
        onPointerMove={(e) => {
          if (e.pointerType !== 'mouse') return e.buttons && tip && setTip(null);
          showTip(e, e.buttons !== 0);
        }}
        onPointerDown={(e) => (tapAt.current = { x: e.clientX, y: e.clientY })}
        onPointerUp={(e) => {
          const t = tapAt.current;
          tapAt.current = null;
          if (e.pointerType !== 'mouse' && t && Math.hypot(e.clientX - t.x, e.clientY - t.y) < 8) showTip(e, false);
        }}
      />
      {tip && (
        <div className="preview-tip" style={{ left: tip.x + 16, top: tip.y + 16 }}>
          <b>{tip.title}</b>
          {tip.body && <div>{tip.body}</div>}
        </div>
      )}
      {restoring && <div className="preview-status" role="status"><span>Restoring the 3D view…</span></div>}
      {status && !restoring && (
        <div className="preview-status" role="status">
          <span>{status}</span>
          {status === 'Not sliced yet' && <button type="button" className="btn-primary" onClick={() => void slice()}>Slice plate</button>}
          {status === 'Plate is empty' && <button type="button" className="btn-primary" onClick={() => { setState({ tab: 'prepare' }); openFileDialog(); }}>Add model</button>}
        </div>
      )}
      {data?.header && (
        <Legend
          data={data}
          ui={ui}
          setUi={setUi}
          loaded={data.loaded}
          ranges={ranges.current}
          scene={sceneRef.current}
          explainOnly={!!grid || methods}
          summary={before ? <Suspense fallback={null}><SliceDiffSummary record={record!} /></Suspense> : undefined}
          headAction={methods ? <Suspense fallback={null}><DoneMethods /></Suspense> : before && <Suspense fallback={null}><DoneComparing /></Suspense>}
          explain={explainKey}
        />
      )}
      {job.state === 'done' && !methods && !data?.header && !data?.error && (
        <div className={phoneSheetQuery.matches ? 'legend closed' : 'legend'} role="status" aria-busy="true" aria-label="Loading toolpaths">
          <div className="lg-head"><Skeleton width={88} height={14} style={{ margin: '7px 0' }} /></div>
          {!phoneSheetQuery.matches && <><div className="lg-summary">
            {[0, 1].map((i) => <div key={i} className="lg-stat"><Skeleton width={84} height={22} style={{ margin: '2px 0' }} /><Skeleton width={56} height={10} style={{ margin: '3px 0' }} /></div>)}
          </div>
          <div className="lg-scroll"><div className="lg-roles">
            {[70, 60, 75, 55, 80, 65, 50, 60].map((w, i) => <div key={i} className="lg-role-row"><Skeleton block width={14} height={6} /><Skeleton width={`${w}%`} height={10} /><Skeleton width={36} height={10} /></div>)}
          </div></div></>}
        </div>
      )}
      {methods && !data?.header && (
        <div className="legend" aria-label="Legend">
          <div className="lg-head"><Suspense fallback={null}><DoneMethods /></Suspense></div>
          <div className="lg-scroll">{explainKey}</div>
        </div>
      )}
      {comparing && data?.header && (
        <Suspense fallback={null}>
          <CompareView source={compare!} data={data} record={record!} scene={sceneRef.current} ui={ui}
            span={[layerShare(data, lo), layerShare(data, hi)]} />
        </Suspense>
      )}
      {layerCount > 0 && (
        <LayerSlider
          layers={data!.layers}
          lo={oneLayer ? 0 : lo}
          hi={hi}
          oneLayer={oneLayer}
          onChange={(a, b) => {
            setLayers(oneLayer ? ([x]) => [x, b] : [a, b]);
            setMovePos(null);
          }}
          ticks={eventTicks(events, data!.layers)}
          onPlus={(anchor) => setEventMenu({ anchor, layer: hi })}
        />
      )}
      {layerCount > 0 && (
        <EventList
          onPick={(z) => {
            const i = data!.layers.findIndex((l) => l.z >= z - 1e-3);
            if (i >= 0) setLayers(([a]) => [Math.min(a, i), i]);
          }}
        />
      )}
      {eventMenu && data && (
        <EventMenu
          anchor={eventMenu.anchor}
          layer={eventMenu.layer}
          z={Math.round(data.layers[eventMenu.layer].z * 1e4) / 1e4}
          onClose={() => setEventMenu(null)}
          onJump={jumpToLayer}
        />
      )}
      {layerCount > 0 && topMoves > 0 && (
        <OrderBar data={data!} at={topFirst + pos - 1} onSeek={seekOrder}
          gcodeOpen={gcodeWindow} onGcode={() => toggleViewOption('gcodeWindow')} oneLayer={oneLayer} onOneLayer={toggleOneLayer} />
      )}
      {layerCount > 0 && pos < topMoves && data!.states && topFirst + pos - 1 < data!.loaded && (
        <Suspense fallback={null}><ToolPosition data={data!} at={topFirst + pos - 1} view={ui.view} /></Suspense>
      )}
      {gcodeWindow && layerCount > 0 && topMoves > 0 && (
        <Suspense fallback={null}><GcodeWindow line={data!.gcode[topFirst + pos - 1]} file={dropped} onClose={() => toggleViewOption('gcodeWindow')} /></Suspense>
      )}
    </div>
  );
}
