import { recordOf } from '../profiles/view';
import type { PrinterRecord } from '../profiles/records';
import { Check } from './Check';
import { MODE_LABELS } from '../nonplanar-mode';
import { Dropdown } from './Dropdown';
import { Caret } from './Caret';
import { memo, useEffect, useLayoutEffect, useRef, useState, type ReactNode } from 'react';
import { effectiveConfig, getState, useStore } from '../state/store';
import {
  CURVED_COLOR, EXTENDED_VIEWS, clearanceColor, NONPLANAR_COLORS, NONPLANAR_HUES, NONPLANAR_PLANAR_COLOR, OPTION_COLORS, RANGE_PALETTE, ROLE_COLORS, VIEW_TYPES, hexToRgb,
  nonplanarIndexColor, type ColorRange, type RGB, type Ranges, type ViewType,
} from '../preview/colors';
import type { PreviewData } from '../preview/data';
import { ROLE_NAMES, spacedTime, type PrintStatistics } from '../preview/format';
import type { PreviewScene } from '../preview/PreviewScene';
import { canExtend, type PreviewUi } from './PreviewView';
import { clearanceScale } from '../preview/clearance';
import { BP } from '../breakpoints';
import { phoneSheetQuery } from '../workspace-sheet';
import { Sep } from '../separator';
import { slotFilaments } from '../profiles/slots';

const css = (c: RGB) => `rgb(${c[0]},${c[1]},${c[2]})`;

const SHOW_STATS = new URLSearchParams(location.search).has('stats');

const spacedWeight = (g: number) => g >= 1000 ? `${(g / 1000).toFixed(2)} kg` : `${g.toFixed(1)} g`;

function compactWeight(g: number): string {
  const a = Math.abs(g);
  const [v, unit] = a >= 1e6 ? [a / 1e6, 't'] : a >= 1e3 ? [a / 1e3, 'kg'] : [a, 'g'];
  return `${g < 0 ? '-' : ''}${v.toFixed(2)}${unit}`;
}


const folds: Record<string, boolean> = { key: true, curved: true, show: false };

export function Fold({ id, title, children }: { id: string; title: string; children: ReactNode }) {
  const [open, setOpen] = useState(folds[id] ?? true);
  return (
    <details className="lg-fold" open={open} onToggle={(e) => { folds[id] = e.currentTarget.open; setOpen(e.currentTarget.open); }}>
      <summary>{title}</summary>
      {open && children}
    </details>
  );
}

function Swatch({ c }: { c: RGB }) {
  return <span className="lg-swatch" style={{ background: css(c) }} />;
}

function RangeRows({ range, decimals, title }: { range: ColorRange | undefined; decimals: number; title?: string }) {
  if (!range) return null;
  const values = range.values();
  const rows = values.length === 2
    ? [[values[1], RANGE_PALETTE.length - 1], [values[0], 0]]
    : values.map((v, i) => [v, i] as [number, number]).reverse();
  return (
    <>
      {title && <div className="lg-sec label-caps">{title}</div>}
      <div className="lg-scale">
        {rows.map(([v, i], k) => (
          <div className="lg-row" key={k}>
            <Swatch c={RANGE_PALETTE[i]} />
            <span className="lg-num">{v.toFixed(decimals)}</span>
          </div>
        ))}
      </div>
    </>
  );
}

function filamentInfo(d: PreviewData, statistics?: PrintStatistics) {
  const s = getState();
  return d.filamentMm.map((mm, i) => {
    const native = statistics?.per_extruder.find((f) => f.extruder_id === i);
    if (native && native.used_filament_mm > 0)
      return { mm: mm ?? 0, grams: (mm ?? 0) * native.used_filament_g / native.used_filament_mm };
    const cfg = effectiveConfig(s, 'filament', Math.min(i, Math.max(0, slotFilaments(s).length - 1)));
    const num = (v: unknown, def: number) => {
      const x = Number(Array.isArray(v) ? v[0] : v);
      return Number.isFinite(x) && x > 0 ? x : def;
    };
    const dia = num(cfg.filament_diameter, 1.75);
    const density = num(cfg.filament_density, 1.24);
    const grams = ((mm ?? 0) * Math.PI * (dia / 2) ** 2 * density) / 1000;
    return { mm: mm ?? 0, grams };
  });
}

export const Legend = memo(function Legend(props: {
  data: PreviewData;
  loaded: number;
  ui: PreviewUi;
  setUi(f: (u: PreviewUi) => PreviewUi): void;
  ranges: Ranges | null;
  scene: PreviewScene | null;
  explain?: ReactNode;
  explainOnly?: boolean;
  summary?: ReactNode;
  headAction?: ReactNode;
}) {
  const { data: d, ui, setUi, ranges } = props;
  const [open, setOpen] = useState(true);
  const panel = useRef<HTMLDivElement>(null);
  useLayoutEffect(() => {
    const area = panel.current?.parentElement;
    if (!area) return;
    let band: number | null = null;
    const fit = () => {
      const w = area.clientWidth, b = !phoneSheetQuery.matches && w >= BP.narrow ? 1 : 0;
      if (w && b !== band) { band = b; setOpen(b === 1); }
    };
    fit();
    const ro = new ResizeObserver(fit);
    ro.observe(area);
    phoneSheetQuery.addEventListener('change', fit);
    return () => { ro.disconnect(); phoneSheetQuery.removeEventListener('change', fit); };
  }, []);
  useLayoutEffect(() => {
    const el = panel.current, views = props.scene?.views;
    if (!el || !views) return;
    const fit = () => {
      const host = props.scene!.renderer.domElement.getBoundingClientRect(), r = el.getBoundingClientRect();
      const slider = el.parentElement?.querySelector('.layer-slider')?.getBoundingClientRect();
      views.setInsets(open && r.width ? r.right - host.left + 12 : 0, slider ? host.right - slider.left : 0);
    };
    fit();
    const ro = new ResizeObserver(fit);
    ro.observe(el);
    if (el.parentElement) ro.observe(el.parentElement);
    return () => { ro.disconnect(); views.setInsets(0, 0); };
  }, [open, props.scene]);
  const total = d.totalTime(0);
  const state = getState();
  const record = d.sourceUrl && state.job.id && d.sourceUrl === state.api?.movesUrl(state.job.id)
    ? state.job.status?.record : undefined;
  const printStats = d.statistics(record);
  const manualSwap = useStore((s) => (recordOf(s, 'machine', s.printer) as PrinterRecord | undefined)?.swap === 'manual');
  const fil = filamentInfo(d, printStats);
  const totalsByExtruder = printStats
    ? [...printStats.per_extruder].sort((a, b) => a.extruder_id - b.extruder_id)
      .map((f) => ({ id: f.extruder_id, mm: f.used_filament_mm, grams: f.used_filament_g }))
    : fil.map((f, id) => ({ ...f, id }));
  const filG = fil.reduce((a, f) => a + f.grams, 0);
  const toggle = (k: keyof PreviewUi['vis']) => setUi((u) => ({ ...u, vis: { ...u.vis, [k]: !u.vis[k] } }));
  const toggleRole = (r: number) =>
    setUi((u) => {
      const h = new Set(u.vis.hiddenRoles);
      if (h.has(r)) h.delete(r);
      else h.add(r);
      return { ...u, vis: { ...u.vis, hiddenRoles: h } };
    });

  const focusRole = (r: number | null) => props.scene?.setFocus(r === null || ui.view !== 'feature' ? null : 1 << r);
  useEffect(() => () => props.scene?.setFocus(null), [props.scene, ui.view]);

  const body = () => {
    if (EXTENDED_VIEWS.has(ui.view) && !d.header?.extended) return <div className="lg-row" role="status">Loading actual speed and clearance…</div>;
    switch (ui.view) {
      case 'feature':
        return (
          <div className="lg-roles" role="group" aria-label="Line types">
            {ROLE_NAMES.map((name, r) => (d.roleTime[r] > 0 || d.roleFilament[r] > 0) && (
              <button key={r} type="button" className="lg-role-row" aria-pressed={!ui.vis.hiddenRoles.has(r)} onClick={() => toggleRole(r)}
                onMouseEnter={() => focusRole(r)} onMouseLeave={() => focusRole(null)} onFocus={() => focusRole(r)} onBlur={() => focusRole(null)}>
                <Swatch c={ROLE_COLORS[r]} /><span className="lg-role-name">{name}</span><span className="lg-role-time">{d.roleTime[r] > 0 ? spacedTime(d.roleTime[r]) : ''}</span>
              </button>
            ))}
          </div>
        );
      case 'height': return <RangeRows range={ranges?.height} decimals={2} />;
      case 'width': return <RangeRows range={ranges?.width} decimals={2} />;
      case 'speed': return <RangeRows range={ranges?.speed} decimals={0} title="Speed (mm/s)" />;
      case 'actual_speed': return <RangeRows range={ranges?.actualSpeed} decimals={0} title="Actual Speed (mm/s)" />;
      case 'actual_flow': return <RangeRows range={ranges?.actualFlow} decimals={2} title="Actual volumetric flow rate (mm³/s)" />;
      case 'clearance': {
        if (!d.clearance.some((v) => !Number.isNaN(v))) return <><div className="lg-sec label-caps">Clearance (mm)</div><div className="lg-row">Not checked</div></>;
        const { margin, floor } = clearanceScale();
        return (
          <>
            <div className="lg-sec label-caps">Clearance (mm)</div>
            {[floor, Math.min(margin, floor), 0].map((v, k) => (
              <div className="lg-row" key={k}><Swatch c={clearanceColor(v, margin, floor)} /><span>{v.toFixed(3)}</span></div>
            ))}
          </>
        );
      }
      case 'acceleration': return <RangeRows range={ranges?.acceleration} decimals={0} title="Acceleration (mm/s²)" />;
      case 'jerk': return <RangeRows range={ranges?.jerk} decimals={0} title="Jerk (mm/s)" />;
      case 'flow': return <RangeRows range={ranges?.flow} decimals={2} title="Flow (mm³/s)" />;
      case 'fan': return <RangeRows range={ranges?.fan} decimals={0} title="Fan Speed (%)" />;
      case 'temperature': return <RangeRows range={ranges?.temperature} decimals={0} title="Temperature (°C)" />;
      case 'pressure_advance': return <RangeRows range={ranges?.pa} decimals={3} />;
      case 'layer_time': return <RangeRows range={ranges?.layerTime} decimals={0} title="Layer Time (s)" />;
      case 'layer_time_log': return <RangeRows range={ranges?.layerTimeLog} decimals={0} title="Layer Time (s)" />;
      case 'filament': {
        const s = getState();
        const cols = (d.meta.extruder_colors?.length ? d.meta.extruder_colors : []).map(hexToRgb);
        const names = d.meta.filament_names?.length ? d.meta.filament_names : slotFilaments(s);
        return (
          <table className="lg-table">
            <tbody>
              {totalsByExtruder.map((f) => (
                <tr key={f.id}>
                  <td><Swatch c={cols[f.id] ?? hexToRgb('#F2754E')} />{f.id + 1} {(names[f.id] ?? '').replace(/\s*@.*$/, '')}</td>
                  <td>{compactWeight(f.grams)}</td>
                </tr>
              ))}
            </tbody>
          </table>
        );
      }
      case 'nonplanar': return <NonplanarKey d={d} ui={ui} setUi={setUi} ranges={ranges} mode={String((record as { inputs?: { mode?: unknown } } | undefined)?.inputs?.mode ?? '')} />;
    }
  };

  const options: [keyof PreviewUi['vis'], string, RGB][] = [
    ['travel', 'Travel', OPTION_COLORS.travel],
    ['wipe', 'Wipe', OPTION_COLORS.wipe],
    ['retract', 'Retract', OPTION_COLORS.retract],
    ['unretract', 'Unretract', OPTION_COLORS.unretract],
    ['seam', 'Seams', OPTION_COLORS.seam],
  ];
  const stats = props.scene?.lastFrame;
  const totalG = printStats?.total_weight_g ?? filG;
  const totalMm = totalsByExtruder.reduce((a, f) => a + f.mm, 0);
  return (
    <div ref={panel} className={open ? 'legend' : 'legend closed'} aria-label="Legend">
      <div className="lg-head">
        <Dropdown className="lg-view" aria-label="Color scheme" value={ui.view} onValueChange={(next) => setUi((u) => ({ ...u, view: next as ViewType }))} options={[...VIEW_TYPES.filter((v) => d.header?.extended || canExtend(d) || !EXTENDED_VIEWS.has(v.key)).map((v) => ({ value: v.key, label: v.label }))]} />
        {props.headAction}
        <button type="button" className="lg-collapse" aria-expanded={open} aria-label={open ? 'Fold legend' : 'Open legend'} onClick={() => setOpen(!open)}> {open ? <Caret dir="up" /> : 'Show'}</button>
      </div>
      {props.summary ?? (
        <div className="lg-summary">
          <div className="lg-stat"><span className="lg-stat-v">{spacedTime(total)}</span><span className="lg-stat-k">Print time</span></div>
          <div className="lg-stat"><span className="lg-stat-v">{spacedWeight(totalG)}</span><span className="lg-stat-k">Filament{totalMm > 0 && <><Sep/>{(totalMm / 1000).toFixed(2)} m</>}</span></div>
          {!!printStats?.total_toolchanges && <div className="lg-stat"><span className="lg-stat-v">{printStats.total_toolchanges}</span><span className="lg-stat-k">{manualSwap ? 'Pauses' : 'Filament changes'}</span></div>}
        </div>
      )}
      {open && (
        <div className="lg-scroll">
          {props.explain}
          {!props.explainOnly && (
            <>
              <div className="lg-body">{body()}</div>
              <Fold id="show" title="Travel, seams and shells">
                <div className="lg-options">
                  {options.map(([k, label, c]) => (
                    <label key={k} className="lg-opt">
                      <Check checked={ui.vis[k] as boolean} onCheckedChange={() => toggle(k)} />
                      <Swatch c={c} />{label}
                    </label>
                  ))}
                  <label className="lg-opt">
                    <Check checked={ui.shells} onCheckedChange={() => setUi((u) => ({ ...u, shells: !u.shells }))} />
                    <span className="lg-swatch blank" />Shells
                  </label>
                </div>
              </Fold>
              {stats && SHOW_STATS && (
                <div className="lg-perf">
                  {stats.blocks} blocks ({stats.lodBlocks} LOD, {stats.culled} culled)<Sep/>{(stats.segments / 1e6).toFixed(2)} M seg<Sep/>{stats.frameMs.toFixed(1)} ms
                  {Number.isFinite(stats.gpuMs) && <><Sep/>GPU {stats.gpuMs.toFixed(1)} ms</>}
                  {d.done && <><Sep/>loaded in {(d.loadMs / 1000).toFixed(2)} s</>}
                </div>
              )}
            </>
          )}
        </div>
      )}
    </div>
  );
});

function NonplanarKey({ d, ui, setUi, ranges, mode }: { d: PreviewData; ui: PreviewUi; setUi(f: (u: PreviewUi) => PreviewUi): void; ranges: Ranges | null; mode?: string }) {
  const st = d.states!;
  const shells = new Set<number>(), bands = new Set<number>(), patches = new Set<number>();
  for (let i = 0; i < st.shell.length; ++i)
    if (st.shell[i] !== 0xff) {
      shells.add(st.shell[i]);
      bands.add(st.band[i]);
      patches.add(st.patch[i]);
    }
  const shellCount = shells.size ? Math.max(...shells) + 1 : 0;
  const indexRows = (values: number[], word: string, plural: string, unlisted: string) => {
    const sorted = [...values].sort((a, b) => a - b);
    const repeat = sorted.length > 0 && sorted[sorted.length - 1] >= NONPLANAR_HUES;
    const rows: { c: RGB; label: string }[] = [];
    if (sorted[0] < 0) rows.push({ c: nonplanarIndexColor(-1), label: unlisted });
    for (let hue = repeat ? NONPLANAR_HUES - 1 : sorted[sorted.length - 1]; hue >= 0; --hue) {
      const marked = sorted.filter((b) => b >= 0 && (repeat ? b % NONPLANAR_HUES === hue : b === hue));
      if (!marked.length) continue;
      const label = marked.slice(0, 3).map((b) => b + 1).join(', ') + (marked.length > 3 ? ', …' : '');
      rows.push({ c: nonplanarIndexColor(hue), label: `${marked.length > 1 ? plural : word} ${label}` });
    }
    return rows;
  };
  let rows: { c: RGB; label: string }[] = [];
  if (ui.nonplanar === 'shell') {
    for (let k = shellCount - 1; k >= 0; --k) rows.push({ c: ranges ? ranges.shell.colorAt(k) : [0, 0, 0], label: `Shell ${k + 1}` });
  } else if (ui.nonplanar === 'band') rows = indexRows([...bands], 'Band', 'Bands', 'Unbanded');
  else if (ui.nonplanar === 'patch') rows = indexRows([...patches], 'Patch', 'Patches', 'No patch');
  else if (ui.nonplanar === 'curved') rows = [{ c: CURVED_COLOR, label: (mode && MODE_LABELS[mode]) || 'Curved' }];
  return (
    <div>
      <Dropdown className="lg-np-by" aria-label="Colour curved passes by" value={NONPLANAR_COLORS.some((c) => c.key === ui.nonplanar) ? ui.nonplanar : 'shell'}
        onValueChange={(next) => setUi((u) => ({ ...u, nonplanar: next as typeof u.nonplanar }))} options={NONPLANAR_COLORS.map((c) => ({ value: c.key, label: c.label }))} />
      {shellCount === 0 && <div className="lg-row lg-dim">No curved passes</div>}
      {shellCount > 0 && rows.map((r, i) => (
        <div className="lg-row" key={i}><Swatch c={r.c} /><span>{r.label}</span></div>
      ))}
      {shellCount > 0 && ui.nonplanar === 'bead_height' && <RangeRows range={ranges?.beadHeight} decimals={2} title="Bead height (mm)" />}
      {shellCount > 0 && ui.nonplanar === 'slope' && <RangeRows range={ranges?.slope} decimals={0} title="Surface slope (°)" />}
      <div className="lg-row"><Swatch c={NONPLANAR_PLANAR_COLOR} /><span>Planar</span></div>
    </div>
  );
}
