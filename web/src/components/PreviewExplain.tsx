import { useEffect, useMemo, useReducer, useRef, useState } from 'react';
import * as THREE from 'three';
import { forgetCompare, setStandardData, showPrevious, startCompare, stopCompare, useCompare, useStandardData, type CompareSource } from '../compare';
import { drawMasks, NONPLANAR_PLANAR_COLOR, type Visibility } from '../preview/colors';
import { loadMoves, supportGrams, type PreviewData } from '../preview/data';
import { area, curvedTopPercent, wholePartLayers, grams, microns, nativeSurfaceSummary, readRecord, supportText, type RecordSummary } from '../preview/explain-record';
import { MoveType, spacedTime } from '../preview/format';
import { PreviewScene } from '../preview/PreviewScene';
import { ERROR_BINS, useSurfaceGrid } from '../preview/surface-grid';
import { plateResults, type PlateResult } from '../recommend';
import { getState, printerPreset, useStore } from '../state/store';
import { colorContext, jobColours, sharedRanges, type PreviewUi } from './PreviewView';
import { Check } from './Check';

const TWO_SCENE_VERTICES = 8e6;
const NOT_PART = new Set([12, 17, 18]);

function partBox(d: PreviewData): [THREE.Vector3, THREE.Vector3] | null {
  const min = new THREE.Vector3(Infinity, Infinity, Infinity), max = new THREE.Vector3(-Infinity, -Infinity, -Infinity);
  const p = new THREE.Vector3();
  for (let i = 1; i < d.loaded; ++i) {
    if ((d.type[i] & 0x7f) !== MoveType.Extrude || NOT_PART.has(d.role[i])) continue;
    p.set(d.pos[i * 4], d.pos[i * 4 + 1], d.pos[i * 4 + 2]);
    min.min(p);
    max.max(p);
  }
  return min.x <= max.x ? [min, max] : null;
}

function curvedShare(s: RecordSummary): number | null {
  const total = s.topCurved + s.topFlat;
  return total > 0 ? Math.min(s.topFlat > 0 ? 99 : 100, Math.max(s.topCurved > 0 ? 1 : 0, Math.round((100 * s.topCurved) / total))) : null;
}

function flatLines(results: PlateResult[]): string[] {
  const flat = results.filter((r) => r.flat);
  const partial = results.filter((r) => r.partial).map((r) => `${results.length > 1 ? `${r.label}: ` : ''}${r.partial![0].toUpperCase()}${r.partial!.slice(1)}`);
  if (!flat.length) return partial;
  const byReason = new Map<string, string[]>();
  for (const r of flat) byReason.set(r.flat!, [...(byReason.get(r.flat!) ?? []), r.label]);
  const named = byReason.size > 1 || flat.length < results.length;
  return [...[...byReason].map(([why, labels]) => `Printed flat: ${named ? `${labels.join(', ')}: ` : ''}${why}`), ...partial];
}

const time = (s: number | null) => (s == null ? '–' : spacedTime(s));

export function ViewSwitches({ ui, setUi, hasSurfaceGrid }: { ui: PreviewUi; setUi(f: (u: PreviewUi) => PreviewUi): void; hasSurfaceGrid: boolean }) {
  const previous = useRef<Pick<PreviewUi, 'view' | 'nonplanar'>>({ view: 'feature', nonplanar: 'shell' });
  const highlight = ui.view === 'nonplanar' && ui.nonplanar === 'curved';
  return <>
    <label className="lg-switch">
      <Check checked={highlight} onCheckedChange={(next) => { const on = next === true;
        if (on) previous.current = { view: ui.view, nonplanar: ui.nonplanar };
        setUi((u) => ({ ...u, ...(on ? { view: 'nonplanar', nonplanar: 'curved' } : previous.current) }));
      }} />Highlight curved
    </label>
    {hasSurfaceGrid && (
      <label className="lg-switch">
        <Check checked={ui.surface} onCheckedChange={(next) => setUi((u) => ({ ...u, surface: next === true }))} />Surface error
      </label>
    )}
  </>;
}

export function ExplainLegend(props: {
  data: PreviewData;
  record: Record<string, unknown>;
  hasSurfaceGrid: boolean;
  ui: PreviewUi;
  setUi(f: (u: PreviewUi) => PreviewUi): void;
  compare: boolean;
  setCompare(on: boolean): void;
}) {
  const { data, record, hasSurfaceGrid, ui, setUi, compare, setCompare } = props;
  const s = useMemo(() => readRecord(record), [record]);
  const results = useStore(plateResults);
  const cmp = useCompare();
  const stdData = useStandardData();
  if (!s || (!s.curved && s.mode === 'off')) return null;
  const std = compare && cmp.state === 'done' ? readRecord(cmp.status.record) : null;
  const curvedPct = curvedShare(s) ?? curvedTopPercent(record);
  const better = (a: number | null | undefined, b: number | null | undefined) => a != null && b != null && b < a / 1.15;
  const timeDelta = std?.timeS && s.timeS ? (s.timeS - std.timeS) / std.timeS : null;
  const v = s.verifier;
  const closest = v?.closest != null && Number.isFinite(v.closest) && v.closest > 0 ? `Closest pass ${v.closest.toFixed(2)} mm` : undefined;
  const pending = !compare ? null : cmp.state === 'running' ? (cmp.queued ? 'waiting' : `${Math.round(cmp.pct)}%`) : cmp.state === 'failed' ? (cmp.busy ? 'busy' : 'failed') : null;

  type Row = [string, string, string, boolean?, string?];
  const rows: Row[] = [];
  if (s.wholePart) rows.push(['Curved layers', '', 'whole part', false, s.wholePart.source]);
  else if (s.refusedWhy) rows.push(['Curved layers', '', 'not used', false, s.refusedWhy]);
  rows.push(['Top error', std ? microns(std.topErrorMm) : pending ?? '', microns(s.topErrorMm), better(std?.topErrorMm, s.topErrorMm),
    better(std?.topErrorMm, s.topErrorMm) ? `${(std!.topErrorMm! / s.topErrorMm!).toFixed(1)}× smoother than Standard` : undefined]);
  const stdPct = std ? curvedShare(std) ?? 0 : null;
  if (s.mode !== 'off' && !s.wholePart) rows.push(['Curved top', stdPct != null ? `${stdPct}%` : '', `${curvedPct ?? 0}%`, stdPct != null && (curvedPct ?? 0) > stdPct]);
  if (s.shells) rows.push(['Curved shells', std ? String(std.shells) : '', String(s.shells)]);
  if (s.overhangs + s.bridges) rows.push(['Curved overhangs', std ? String(std.overhangs + std.bridges) : '', String(s.overhangs + s.bridges), false, `${s.overhangs} overhangs, ${s.bridges} bridges`]);
  rows.push(['Time', std ? time(std.timeS) : '', time(s.timeS) + (timeDelta != null ? ` (${timeDelta >= 0 ? '+' : ''}${Math.round(timeDelta * 100)}%)` : '')]);
  if (s.grams != null) rows.push(['Filament', std ? grams(std.grams) : '', grams(s.grams)]);
  rows.push(['Support', std ? supportText(supportGrams(stdData, std.grams)) : '', supportText(supportGrams(data, s.grams))]);
  if (v) rows.push(['Head hits', '', String(v.hits), false, closest]);
  if (std?.curved) rows.push(['Result', 'not flat', '', false, 'Standard mode still used curved passes here']);
  else if (std && !better(std.topErrorMm, s.topErrorMm)) {
    const faster = timeDelta != null && timeDelta > 0.01;
    rows.push(['Result', '', faster ? 'Standard faster' : 'same', false,
      faster ? 'Standard flat layers are faster here and look just as smooth' : 'Prints identically in both modes on this part']);
  }

  (window as unknown as { __compare: unknown }).__compare = { state: cmp, summary: s, standard: std, stdData, rows };

  const summary = nativeSurfaceSummary(record);
  const flat = flatLines(results);
  return (
    <section className="lg-section np-legend" aria-label="Curved">
      <div className="np-head">
      <div className="lg-section-title">Curved</div>
      {summary && <div className="np-summary-line" role="status">{summary}</div>}
      <div className="np-toggles">
        <label className="lg-switch"><Check checked={compare} onCheckedChange={(next) => setCompare(next === true)} />Compare with Standard</label>
        <ViewSwitches ui={ui} setUi={setUi} hasSurfaceGrid={hasSurfaceGrid} />
      </div>
      </div>
      {flat.map((line) => <div className="lg-row np-flat" role="status" key={line}>{line}</div>)}
      {ui.surface && hasSurfaceGrid ? (
        <>
          <div className="lg-sec label-caps">Top error (µm)</div>
          {[...ERROR_BINS].reverse().map((b) => (
            <div className="lg-row" key={b.label}><span className="lg-swatch" style={{ background: `rgb(${b.color.join(',')})` }} />{b.label}</div>
          ))}
        </>
      ) : <>
      {compare && <table className="lg-table np-table">
        <thead>
          <tr>
            <th><span className="lg-hidden">Measure</span></th>
            {compare && <th>Standard</th>}
            <th>{s.style}</th>
          </tr>
        </thead>
        <tbody>
          {rows.map(([label, a, b, teal]) => (
            <tr key={label}>
              <td>{label}</td>
              {compare && <td>{a}</td>}
              <td className={teal ? 'np-good' : undefined}>{b}</td>
            </tr>
          ))}
        </tbody>
      </table>}
      {compare && cmp.state === 'running' && (
        <button className="lg-mode" onClick={() => setCompare(false)}>Cancel Standard</button>
      )}
      {compare && cmp.state === 'failed' && (
        <button className="lg-mode" onClick={() => void startCompare()}>
          {cmp.busy ? 'Server busy — retry' : 'Flat comparison failed — retry'}
        </button>
      )}
      {compare && v?.uncalibrated && (
        <div className="lg-row np-warn">Print head not calibrated yet</div>
      )}
      {compare && s.flat.length > 0 && (
        <>
          <div className="lg-sec label-caps">Flat areas</div>
          {s.flat.map((f) => (
            <div className="lg-row" key={f.name}>
              <span className="lg-swatch" style={{ background: `rgb(${NONPLANAR_PLANAR_COLOR.join(',')})` }} />
              {f.name} {area(f.area)}
            </div>
          ))}
        </>
      )}
      </>}
    </section>
  );
}

export function CompareView(props: {
  source: CompareSource;
  data: PreviewData;
  record: Record<string, unknown>;
  scene: PreviewScene | null;
  ui: PreviewUi;
  span: [number, number];
}) {
  const { source, data, record, scene, ui } = props;
  const cmp = useCompare();
  const style = useMemo(() => readRecord(record)?.style ?? '', [record]);
  useEffect(() => {
    if (source === 'previous') showPrevious();
    else void startCompare();
  }, [record, source]);
  useEffect(() => () => stopCompare(), []);
  useEffect(() => {
    if (!scene || !data.done) return;
    const box = partBox(data);
    if (box) scene.fit(...box);
  }, [scene, data, data.done]);
  return (
    <>
      <StandardPane main={scene} mainData={data} cmp={cmp} ui={ui} vis={ui.vis} span={props.span} />
      <div className="cmp-label cmp-left label-caps">{source === 'previous' ? 'Before' : 'Standard'}</div>
      <div className="cmp-label cmp-right label-caps">{source === 'previous' ? 'After' : style}</div>
    </>
  );
}

function StandardPane(props: {
  main: PreviewScene | null;
  cmp: ReturnType<typeof useCompare>;
  mainData: PreviewData;
  ui: PreviewUi;
  vis: Visibility;
  span: [number, number];
}) {
  const { main, mainData, cmp, ui, vis, span } = props;
  const host = useRef<HTMLDivElement>(null);
  const [scene, setScene] = useState<PreviewScene | null>(null);
  const [data, setData] = useState<PreviewData | null>(null);
  const [tooLarge, setTooLarge] = useState(false);
  const [gone, setGone] = useState(false);
  const [tip, setTip] = useState<{ text: string; x: number; y: number } | null>(null);
  const grid = useSurfaceGrid(cmp.state === 'done' ? cmp.status : undefined, ui.surface);
  useEffect(() => scene?.setSurface(grid), [scene, grid]);
  const [, bump] = useReducer((x: number) => x + 1, 0);
  const id = cmp.state === 'done' ? cmp.id : null;

  useEffect(() => {
    if (!main) return;
    const sc = new PreviewScene(host.current!);
    sc.setBed(printerPreset(getState()));
    sc.copyCamera(main);
    (window as unknown as { __compareScene: unknown }).__compareScene = sc;
    let syncing = false;
    const follow = (to: PreviewScene, from: PreviewScene) => () => {
      if (syncing) return;
      syncing = true;
      to.copyCamera(from);
      syncing = false;
    };
    const a = follow(sc, main), b = follow(main, sc);
    main.controls.addEventListener('change', a);
    sc.controls.addEventListener('change', b);
    setScene(sc);
    return () => {
      main.controls.removeEventListener('change', a);
      sc.controls.removeEventListener('change', b);
      sc.dispose();
      setScene(null);
    };
  }, [main]);

  useEffect(() => {
    if (!scene || !id || cmp.state !== 'done') return;
    const api = getState().api!;
    let skip = false;
    const h = loadMoves({ url: api.movesUrl(id), shellLayers: wholePartLayers(cmp.status.record) }, {
      header: (d) => {
        skip = d.n + mainData.n > TWO_SCENE_VERTICES;
        setTooLarge(skip);
        if (skip) return;
        scene.setData(d);
        setData(d);
      },
      chunk: () => !skip && scene.sync(),
      done: (d) => {
        setStandardData(d);
        if (skip) return;
        scene.setColors(colorContext(d, ui, sharedRanges(d, mainData, ui)));
        scene.sync();
        bump();
      },
      error: (d) => {
        if (/^404 /.test(d.error ?? '')) {
          if (cmp.source === 'previous') return setGone(true);
          forgetCompare(cmp.key);
          void startCompare();
        }
      },
    }, jobColours());
    return () => {
      h.cancel();
      setStandardData(null);
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [scene, id]);

  useEffect(() => {
    if (scene && data?.done) scene.setColors(colorContext(data, ui, sharedRanges(data, mainData, ui)));
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [ui.view, ui.nonplanar, vis, mainData, mainData.done]);

  useEffect(() => {
    if (!scene || !data?.layers.length) return;
    const L = data.layers, z0 = L[0].z, z1 = L[L.length - 1].z;
    const at = (f: number) => z0 + f * (z1 - z0);
    const lo = span[0] <= 0 ? 0 : Math.max(0, L.findIndex((l) => l.z >= at(span[0]) - 1e-3));
    let hi = L.length - 1;
    if (span[1] < 1) while (hi > lo && L[hi].z > at(span[1]) + 1e-3) --hi;
    scene.setDrawState({ range: data.layerSpan(lo, hi), ...drawMasks(vis, !!grid) }, null);
  }, [scene, data, data?.loaded, data?.done, span[0], span[1], vis, grid]);

  return (
    <div ref={host} className="preview-canvas cmp-std" onPointerLeave={() => setTip(null)} onPointerMove={(e) => {
      if (!grid || !scene || e.buttons) return tip && setTip(null);
      const r = host.current!.getBoundingClientRect();
      const x = e.clientX - r.left, y = e.clientY - r.top, cell = scene.pickSurface(x, y);
      setTip(cell < 0 ? null : { text: `Top error ${Math.round(grid.rms[cell] * 1000)} µm`, x, y });
    }}>
      {tooLarge && <div className="preview-status">Too large for side-by-side view</div>}
      {gone && <div className="preview-status">The previous slice is no longer cached on the server</div>}
      {tip && <div className="preview-tip" style={{ left: tip.x + 16, top: tip.y + 16 }}><b>{tip.text}</b></div>}
    </div>
  );
}
