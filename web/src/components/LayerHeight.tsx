import { Check } from './Check';
import { Range } from './Range';
import { More } from './kit';
import { useRef, useState } from 'react';
import { effectiveConfig, getState, setState, useStore, type PlateObject, type State } from '../state/store';
import { beginStep, undoable } from '../state/history';
import { instanceMatrix, objectHeight } from '../plater/geometry';
import { adaptiveProfile, adjustProfile, baseProfile, fitProfile, heightAt, slicingParams, smoothProfile, type EditAction, type SlicingParams } from '../plater/layerProfile';
import { firstLayer } from '../slice-input';

export const canEditLayers = (s: State) => !!s.selected && !s.selected.all && !s.project &&
  !!s.objects.find((o) => o.id === s.selected!.object && !o.project);

const THICKNESS_BAR_WIDTH = 70;
const STRENGTH = 0.005;

export function layerParams(s: State, o: PlateObject): SlicingParams {
  const proc = { ...effectiveConfig(s, 'process'), ...o.settings }, machine = effectiveConfig(s, 'machine');
  const i = Math.max(0, o.extruder - 1);
  const at = (v: unknown) => Number(Array.isArray(v) ? v[Math.min(i, v.length - 1)] : v) || 0;
  return slicingParams(Number(proc.layer_height) || 0.2, firstLayer(s), at(machine.min_layer_height), at(machine.max_layer_height), at(machine.nozzle_diameter) || 0.4, objectHeight(o));
}

export const layerProfileOf = (s: State, o: PlateObject, p = layerParams(s, o)) => (o.layerProfile ? fitProfile(o.layerProfile, p.height, p.first) : baseProfile(p));

function setProfile(id: number, layerProfile: number[] | undefined) {
  setState((s) => ({ objects: s.objects.map((o) => (o.id === id ? { ...o, layerProfile } : o)) }));
}

let cursor: { z: number; band: number } | null = null;
const cursorListeners = new Set<() => void>();
export const layerCursor = () => cursor;
export const subscribeLayerCursor = (l: () => void) => (cursorListeners.add(l), () => void cursorListeners.delete(l));
function setCursor(next: typeof cursor) {
  if (next?.z === cursor?.z && next?.band === cursor?.band) return;
  cursor = next;
  cursorListeners.forEach((l) => l());
}

const useLayerObject = () => useStore((s) => (s.gizmo === 'layers' ? s.objects.find((o) => o.id === s.selected?.object) : undefined));

export function LayerHeightPanel() {
  const o = useLayerObject();
  const [quality, setQuality] = useState(0.5);
  const [radius, setRadius] = useState(5);
  const [keepMin, setKeepMin] = useState(false);
  if (!o) return null;
  const adaptive = () => {
    const s = getState(), p = layerParams(s, o), g = o.geometry;
    undoable('Variable layer height - Adaptive', () => setProfile(o.id, adaptiveProfile(p, g.attributes.position.array, g.index?.array ?? null, instanceMatrix(o, o.instances[0]), quality)));
  };
  const smooth = () => {
    const s = getState(), p = layerParams(s, o);
    undoable('Variable layer height - Smooth all', () => setProfile(o.id, smoothProfile(layerProfileOf(s, o, p), p, radius, keepMin)));
  };
  return (
    <div className="manip-panel">
      <div className="manip-title">Variable layer height</div>
      <div className="manip-grid" style={{ gridTemplateColumns: 'auto auto auto' }}>
        <span>Quality / Speed</span>
        <Range min={0} max={1} step={0.05} value={quality} onValueChange={(next) => setQuality(next)} />
        <span>{quality.toFixed(2)}</span>
      </div>
      <div style={{ display: 'flex', gap: 8 }}>
        <button className="btn" onClick={adaptive}>Adaptive</button>
        <button className="btn" onClick={smooth}>Smooth</button>
      </div>
      <More id="layer-height">
        <div className="manip-grid" style={{ gridTemplateColumns: 'auto auto auto' }}>
          <span>Radius</span>
          <Range min={1} max={10} step={1} value={radius} onValueChange={(next) => setRadius(next)} />
          <span>{radius}</span>
          <span>Keep min</span>
          <Check checked={keepMin} onCheckedChange={(next) => setKeepMin(next)} style={{ justifySelf: 'start' }} />
          <span />
        </div>
      </More>
      <div style={{ display: 'flex', gap: 8, marginTop: 8 }}>
        <button className="btn" disabled={!o.layerProfile} onClick={() => undoable('Variable layer height - Reset', () => setProfile(o.id, undefined))}>Reset</button>
        <button className="btn primary" onClick={() => setState({ gizmo: 'none' })}>Done</button>
      </div>
    </div>
  );
}

export function LayerHeightBar() {
  const o = useLayerObject();
  const s = useStore((st) => st);
  const ref = useRef<SVGSVGElement>(null);
  const edit = useRef<{ end: () => void; timer: number; z: number; action: EditAction } | null>(null);
  const [band, setBand] = useState(2);
  const [hoverZ, setHoverZ] = useState<number | null>(null);
  if (!o) return null;
  const p = layerParams(s, o), profile = layerProfileOf(s, o, p);
  const zAt = (clientY: number) => {
    const r = ref.current!.getBoundingClientRect();
    return Math.min(p.height, Math.max(0, (p.height * (r.bottom - clientY - 1)) / r.height));
  };
  const apply = () => {
    const e = edit.current, cur = getState().objects.find((ob) => ob.id === o.id);
    if (!e || !cur) return;
    const pp = layerParams(getState(), cur);
    setProfile(o.id, adjustProfile(layerProfileOf(getState(), cur, pp), pp, e.z, STRENGTH, band, e.action));
  };
  const down = (ev: React.PointerEvent) => {
    if (ev.button !== 0 && ev.button !== 2) return;
    ev.preventDefault();
    ref.current!.setPointerCapture(ev.pointerId);
    const action: EditAction = ev.shiftKey ? (ev.button === 2 ? 'smooth' : 'reduce') : ev.button === 2 ? 'increase' : 'decrease';
    edit.current = { end: beginStep('Variable layer height - Manual edit'), timer: window.setInterval(apply, 300), z: zAt(ev.clientY), action };
    apply();
  };
  const up = () => {
    if (!edit.current) return;
    clearInterval(edit.current.timer);
    edit.current.end();
    edit.current = null;
  };
  const W = THICKNESS_BAR_WIDTH, H = 1000, x = (h: number) => (h / p.max) * (W - 6) + 3, y = (z: number) => H - (z / p.height) * H;
  const points: string[] = [];
  for (let z = 0; z <= p.height; z += p.height / 400) points.push(`${x(heightAt(profile, z))},${y(z)}`);
  return (
    <svg ref={ref} className="layer-bar" viewBox={`0 0 ${W} ${H}`} preserveAspectRatio="none" style={{ width: W }}
      onPointerDown={down} onPointerUp={up} onPointerCancel={up} onContextMenu={(e) => e.preventDefault()}
      onPointerMove={(ev) => { setHoverZ(zAt(ev.clientY)); setCursor({ z: zAt(ev.clientY), band }); if (edit.current) { edit.current.z = zAt(ev.clientY); apply(); } }}
      onPointerLeave={() => { setHoverZ(null); setCursor(null); }}
      onWheel={(ev) => setBand((b) => { const nb = Math.max(1.5, Math.min(10, b * (1 - 0.1 * Math.sign(ev.deltaY)))); if (cursor) setCursor({ z: cursor.z, band: nb }); return nb; })}>
      <title>{`${p.min.toFixed(2)} – ${p.max.toFixed(2)} mm`}</title>
      <rect x={0} y={0} width={W} height={H} className="layer-bar-bg" />
      {hoverZ !== null && <rect x={0} y={y(Math.min(p.height, hoverZ + band / 2))} width={W} height={(band / p.height) * H} className="layer-bar-band" />}
      <line x1={x(p.layer)} x2={x(p.layer)} y1={0} y2={H} className="layer-bar-base" vectorEffect="non-scaling-stroke" />
      <polyline points={points.join(' ')} className="layer-bar-profile" vectorEffect="non-scaling-stroke" />
    </svg>
  );
}
