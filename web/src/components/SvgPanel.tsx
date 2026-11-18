import { Check } from './Check';
import { Dropdown } from './Dropdown';
import { useEffect, useRef, useState, useSyncExternalStore } from 'react';
import { changeVolumeType, setState, useStore, type SvgSpec } from '../state/store';
import { editedSvg, updateSvg } from '../plater/svg';
import { setTextPlacing, subscribeTextPlacing, textPlacing } from '../plater/textPlacing';

const OPERATIONS = [['part', 'Join'], ['negative', 'Cut'], ['modifier', 'Modifier']] as const;

export default function SvgPanel() {
  const objectId = useStore((s) => editedSvg(s)?.object.id);
  const v = useStore((s) => editedSvg(s)?.volume);
  const current = useStore((s) => editedSvg(s)?.spec);
  const placing = useSyncExternalStore(subscribeTextPlacing, textPlacing);
  const [spec, setSpec] = useState<SvgSpec | null>(current ?? null);
  const key = objectId !== undefined ? `${objectId}:${v?.id ?? ''}` : '';
  const timer = useRef(0);
  useEffect(() => () => setTextPlacing(false), []);
  useEffect(() => setSpec(current ?? null), [key]); // eslint-disable-line react-hooks/exhaustive-deps
  useEffect(() => setSpec((sp) => (sp && current ? { ...sp, surface: current.surface } : sp)), [current?.surface]); // eslint-disable-line react-hooks/exhaustive-deps
  if (objectId === undefined || !spec) return null;
  const edit = (patch: Partial<SvgSpec>) => {
    const next = { ...spec, ...patch };
    setSpec(next);
    clearTimeout(timer.current);
    timer.current = window.setTimeout(() => void updateSvg(next), 400);
  };
  const num = (label: string, k: 'depth' | 'size', min: number, max: number) => (
    <label className="prefs-row cut-joint-row"><span>{label}</span>
      <input className="text num" type="number" min={min} max={max} step={0.1} value={spec[k] ?? ''} aria-label={label}
        onChange={(e) => { const n = Number(e.target.value); if (Number.isFinite(n) && n > 0) edit({ [k]: Math.min(max, Math.max(min, n)) }); }} />
      <span>mm</span>
    </label>
  );
  return (
    <div className="manip-panel brim-ears text-panel">
      <div className="manip-title">SVG</div>
      <div className="prefs-row cut-joint-row"><span>File</span><span title={spec.name}>{spec.name}.svg</span></div>
      {num('Depth', 'depth', 0.01, 1000)}
      {num('Size', 'size', 0.1, 1000)}
      {v && (
        <label className="prefs-row cut-joint-row">
          <span>Use surface</span>
          <Check checked={!!spec.surface || placing}
            onCheckedChange={(next) => { if (next) setTextPlacing(true); else { setTextPlacing(false); if (spec.surface) edit({ surface: undefined }); } }} />
        </label>
      )}
      {placing && <div className="cut-joint-hint">Click anywhere on the model's surface to place your SVG.</div>}
      {v && (
        <label className="prefs-row cut-joint-row"><span>Operation</span>
          <Dropdown className="combo" value={v.type} aria-label="Operation"
            onValueChange={(next) => { changeVolumeType(objectId, v.id, next as typeof v.type); if (spec.surface) void updateSvg(spec); }} options={[...OPERATIONS.map(([t, label]) => ({ value: t, label, disabled: false }))]} />
        </label>
      )}
      <div className="cut-joint-buttons"><span style={{ flex: 1 }} /><button type="button" className="btn primary" onClick={() => setState({ gizmo: 'none' })}>Done</button></div>
    </div>
  );
}
