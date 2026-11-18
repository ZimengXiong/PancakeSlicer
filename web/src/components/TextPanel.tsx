import { Check } from './Check';
import { Dropdown } from './Dropdown';
import { More } from './kit';
import { useEffect, useRef, useState, useSyncExternalStore } from 'react';
import { changeVolumeType, setState, useStore, type TextSpec } from '../state/store';
import { editedText, loadFonts, updateText, type Font } from '../plater/text';
import { setTextPlacing, subscribeTextPlacing, textPlacing } from '../plater/textPlacing';

const OPERATIONS = [['part', 'Join'], ['negative', 'Cut'], ['modifier', 'Modifier']] as const;

export default function TextPanel() {
  const objectId = useStore((s) => editedText(s)?.object.id);
  const v = useStore((s) => editedText(s)?.volume);
  const current = useStore((s) => editedText(s)?.spec);
  const [fonts, setFonts] = useState<Font[]>([]);
  const placing = useSyncExternalStore(subscribeTextPlacing, textPlacing);
  useEffect(() => () => setTextPlacing(false), []);
  const [spec, setSpec] = useState<TextSpec | null>(current ?? null);
  const key = objectId !== undefined ? `${objectId}:${v?.id ?? ''}` : '';
  const timer = useRef(0);
  useEffect(() => void loadFonts().then(setFonts), []);
  useEffect(() => setSpec(current ?? null), [key]); // eslint-disable-line react-hooks/exhaustive-deps
  useEffect(() => setSpec((sp) => (sp && current ? { ...sp, surface: current.surface } : sp)), [current?.surface]); // eslint-disable-line react-hooks/exhaustive-deps
  if (objectId === undefined || !spec) return null;
  const edit = (patch: Partial<TextSpec>) => {
    const next = { ...spec, ...patch };
    setSpec(next);
    clearTimeout(timer.current);
    timer.current = window.setTimeout(() => void updateText(next), 400);
  };
  const num = (label: string, k: 'size' | 'depth' | 'charGap' | 'lineSpacing', min: number, max: number, step: number, unit = '') => (
    <label className="prefs-row cut-joint-row"><span>{label}</span>
      <input className="text num" type="number" min={min} max={max} step={step} value={spec[k]} aria-label={label}
        onChange={(e) => { const v = Number(e.target.value); if (Number.isFinite(v)) edit({ [k]: Math.min(max, Math.max(min, v)) }); }} />
      <span>{unit}</span>
    </label>
  );
  return (
    <div className="manip-panel brim-ears text-panel">
      <div className="manip-title">Text</div>
      <textarea className="text" rows={3} value={spec.text} aria-label="Text" onChange={(e) => edit({ text: e.target.value })} />
      {!spec.text.trim() && <div className="brim-warning">Please enter some visible text (not just spaces!).</div>}
      <label className="prefs-row cut-joint-row"><span>Font</span>
        <Dropdown className="combo" value={spec.font} aria-label="Font" onValueChange={(next) => edit({ font: next })} options={[{ value: "", label: 'Default' }, ...fonts.map((f) => ({ value: f.id, label: f.name }))]} />
      </label>
      {num('Height', 'size', 0.1, 1000, 0.1, 'mm')}
      <div className="prefs-row cut-joint-row"><span>Style</span>
        <span className="cut-joint-buttons">
          <button type="button" className={`btn ${spec.bold ? 'primary' : ''}`} aria-pressed={spec.bold} onClick={() => edit({ bold: !spec.bold })}><b>B</b></button>
          <button type="button" className={`btn ${spec.italic ? 'primary' : ''}`} aria-pressed={spec.italic} onClick={() => edit({ italic: !spec.italic })}><i>I</i></button>
        </span>
      </div>
      {v && (
        <label className="prefs-row cut-joint-row"><span>Operation</span>
          <Dropdown className="combo" value={v.type} aria-label="Operation" onValueChange={(next) => { changeVolumeType(objectId, v.id, next as typeof v.type); if (spec.surface) void updateText(spec);  }} options={[...OPERATIONS.map(([t, label]) => ({ value: t, label, disabled: false }))]} />
        </label>
      )}
      <More id="text">
        {num('Depth', 'depth', 0.01, 1000, 0.1, 'mm')}
        <label className="prefs-row cut-joint-row"><span>Alignment</span>
          <Dropdown className="combo" value={spec.align} aria-label="Alignment" onValueChange={(next) => edit({ align: next as TextSpec['align'] })} options={[{ value: "left", label: 'Left' }, { value: "center", label: 'Center' }, { value: "right", label: 'Right' }]} />
        </label>
        {num('Char gap', 'charGap', -100, 100, 1)}
        {num('Line gap', 'lineSpacing', 0.1, 10, 0.1)}
        {v && (
          <label className="prefs-row cut-joint-row">
            <span>Use surface</span>
            <Check checked={!!spec.surface || placing}
              onCheckedChange={(next) => { if (next) setTextPlacing(true); else { setTextPlacing(false); if (spec.surface) edit({ surface: undefined }); } }} />
          </label>
        )}
        {placing && <div className="cut-joint-hint">Click anywhere on the model's surface to place your text.</div>}
      </More>
      <div className="cut-joint-buttons"><span style={{ flex: 1 }} /><button type="button" className="btn primary" onClick={() => setState({ gizmo: 'none' })}>Done</button></div>
    </div>
  );
}
