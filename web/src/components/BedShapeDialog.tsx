import { Dropdown } from './Dropdown';
import { useId, useState } from 'react';
import { Modal, ModalBody, ModalFooter, ModalTitle } from './Modal';
import type { Config, ConfigValue } from '../api/types';
import { bedBounds, parsePoints } from '../plater/geometry';
import { bedAsset, registerBedAsset } from '../plater/bed-assets';
import { STLLoader } from 'three/examples/jsm/loaders/STLLoader.js';
import { union, type Polygon, type Pair } from 'polygon-clipping';
import { setOption, toast } from '../state/store';
import { InfoTip } from './InfoTip';

const SIZE_MIN = 0.01, SIZE_MAX = 214700, ORIGIN_LIMIT = 107350;

export default function BedShapeDialog({ config, close, onSet = (key, value) => setOption('machine', key, value) }: { config: Config; close(): void; onSet?(key: string, value: ConfigValue): void }) {
  const id = useId();
  const initial = parsePoints(config.printable_area);
  const bounds = bedBounds(initial);
  const [custom, setCustom] = useState(initial.map((p) => [p.x, p.y]));
  const [error, setError] = useState('');
  const [shape, setShape] = useState(initial.length === 4 && initial.every((p) => (p.x === bounds.min.x || p.x === bounds.max.x) && (p.y === bounds.min.y || p.y === bounds.max.y)) ? 'Rectangular' : initial.length === 72 && initial.every((p) => Math.abs(p.length() - initial[0].length()) < 0.01) ? 'Circular' : 'Custom');
  const [size, setSize] = useState([bounds.max.x - bounds.min.x, bounds.max.y - bounds.min.y]);
  const [origin, setOrigin] = useState([-bounds.min.x, -bounds.min.y]);
  const [diameter, setDiameter] = useState(size[0]);
  const [texture, setTexture] = useState(String(config.bed_custom_texture ?? ''));
  const [model, setModel] = useState(String(config.bed_custom_model ?? ''));
  const validSize = (n: number) => Number.isFinite(n) && n >= SIZE_MIN && n <= SIZE_MAX;
  const valid = shape === 'Rectangular' ? size.every(validSize) && origin.every((n) => Number.isFinite(n) && Math.abs(n) <= ORIGIN_LIMIT) : shape === 'Circular' ? validSize(diameter) : custom.length >= 3;
  const points = shape === 'Rectangular' ? [[-origin[0], -origin[1]], [size[0] - origin[0], -origin[1]], [size[0] - origin[0], size[1] - origin[1]], [-origin[0], size[1] - origin[1]]] :
    shape === 'Circular' ? Array.from({ length: 72 }, (_, i) => { const a = (i + 1) * Math.PI * 2 / 72; return [diameter / 2 * Math.cos(a), diameter / 2 * Math.sin(a)]; }) : custom;
  const xs = points.map((p) => p[0]), ys = points.map((p) => p[1]);
  const minX = Math.min(0, ...xs), minY = Math.min(0, ...ys), maxX = Math.max(0, ...xs), maxY = Math.max(0, ...ys);
  const scale = 230 / Math.max(maxX - minX, maxY - minY, 1);
  const px = (n: number) => 30 + (n - minX) * scale;
  const py = (n: number) => 265 - (n - minY) * scale;
  const num = (label: string, value: number, set: (v: number) => void, min: number, max: number) => {
    const reason = !Number.isFinite(value) ? 'Enter a number' : value < min ? `Min ${min} mm` : value > max ? `Max ${max} mm` : '';
    const errorId = `${id}-${label.replace(/ /g, '-')}`;
    return <label className="form-row"><span>{label}<InfoTip label={label}>Allowed range: {min} mm to {max} mm</InfoTip></span>
      <span className="control-stack"><span className="modal-unit"><input className="text num" type="number" min={min} max={max} step="any" value={Number.isFinite(value) ? value : ''} aria-invalid={!!reason || undefined} aria-describedby={reason ? errorId : undefined}
        onChange={(e) => set(e.target.valueAsNumber)} onBlur={() => { if (reason) toast('kept_previous_value', { label, reason }); }} /><span>mm</span></span>
        {reason && <span className="control-error" id={errorId} role="status">{reason}</span>}</span></label>;
  };
  const at = (values: number[], set: (v: number[]) => void, i: number) => (v: number) => set(values.map((x, j) => (i === j ? v : x)));
  const asset = (title: string, value: string, set: (v: string) => void, accept: string) => <div className="form-row"><span>{title}</span>
    <span className="modal-file">{value ? <span className="modal-file-name" data-full-text={value}>{value.split(/[\\/]/).pop()}</span> : <span className="modal-file-none">None</span>}
      <label className="btn">Choose…<input type="file" aria-label={`Choose ${title.toLowerCase()}`} accept={accept} hidden onChange={(e) => { const file = e.target.files?.[0]; if (file) set(registerBedAsset(file)); }} /></label>
      {value && <button className="btn-ghost" type="button" aria-label={`Remove ${title.toLowerCase()}`} onClick={() => set('')}>Remove</button>}</span></div>;
  const loadShape = async (file: File) => {
    try {
      const geometry = new STLLoader().parse(await file.arrayBuffer());
      try {
        const pos = geometry.getAttribute('position');
        const triangles: Polygon[] = [];
        for (let i = 0; i < pos.count; i += 3) {
          const p = [0, 1, 2].map((j) => [pos.getX(i + j), pos.getY(i + j)] as Pair);
          const area = (p[1][0] - p[0][0]) * (p[2][1] - p[0][1]) - (p[2][0] - p[0][0]) * (p[1][1] - p[0][1]);
          if (Math.abs(area) > 1e-8) triangles.push([p]);
        }
        if (!triangles.length) throw new Error('Hmm, that file doesn\'t seem to have any 3D geometry in it.');
        let merged = union(triangles[0]);
        for (let i = 1; i < triangles.length; i += 128) merged = union(merged, ...triangles.slice(i, i + 128));
        if (merged.length !== 1) throw new Error('Oops! That file contains multiple disconnected regions, which isn\'t supported here.');
        setCustom(merged[0][0].slice(0, -1));
        setError('');
      } finally { geometry.dispose(); }
    } catch (e) { setError(e instanceof Error ? e.message : 'Error: invalid model'); }
  };
  const invalid = !valid && (shape === 'Custom' ? 'Load an STL model of your print bed.' : 'Please enter bed dimensions greater than 0 mm.');
  return <Modal size="l" label="Bed shape" onClose={close}>
    <form onSubmit={(e) => { e.preventDefault(); if (!valid) return; onSet('printable_area', points.map(([x, y]) => `${Number(x.toFixed(6))}x${Number(y.toFixed(6))}`)); onSet('bed_custom_texture', texture); onSet('bed_custom_model', model); close(); }}>
      <ModalTitle title="Bed shape" onClose={close} />
      <ModalBody className="modal-split"><div className="modal-fields">
        <div className="form-row"><span>Shape</span><Dropdown className="combo" aria-label="Shape" value={shape} onValueChange={(next) => setShape(next as typeof shape)} options={[{ value: 'Rectangular', label: 'Rectangle' }, { value: 'Circular', label: 'Circle' }, { value: 'Custom', label: 'Custom' }]} /></div>
        {shape === 'Rectangular' && <>{num('Width', size[0], at(size, setSize, 0), SIZE_MIN, SIZE_MAX)}{num('Depth', size[1], at(size, setSize, 1), SIZE_MIN, SIZE_MAX)}
          {num('Origin X', origin[0], at(origin, setOrigin, 0), -ORIGIN_LIMIT, ORIGIN_LIMIT)}{num('Origin Y', origin[1], at(origin, setOrigin, 1), -ORIGIN_LIMIT, ORIGIN_LIMIT)}</>}
        {shape === 'Circular' && num('Diameter', diameter, setDiameter, SIZE_MIN, SIZE_MAX)}
        {shape === 'Custom' && <div className="form-row"><span>Outline</span><span className="modal-file"><span className="modal-file-none">{custom.length >= 3 ? `${custom.length} points` : 'None'}</span>
          <label className="btn">Load STL…<input type="file" aria-label="Load shape from STL" hidden accept=".stl" onChange={(e) => { const file = e.target.files?.[0]; if (file) void loadShape(file); }} /></label></span></div>}
        {asset('Texture', texture, setTexture, '.png,.svg')}{asset('Model', model, setModel, '.stl')}
        {(error || invalid) && <p className="modal-error" role="alert">{error || invalid}</p>}
      </div>
      <svg className="bed-2d" viewBox="0 0 300 300" role="img" aria-label="Bed shape preview">
        <defs><pattern id="bed-grid" width={10 * scale || 10} height={10 * scale || 10} patternUnits="userSpaceOnUse"><path d={`M ${10 * scale} 0 L 0 0 0 ${10 * scale}`} fill="none" className="bed-2d-grid" strokeWidth="0.5" /></pattern><clipPath id="bed-clip"><polygon points={points.map(([x, y]) => `${px(x)},${py(y)}`).join(' ')} /></clipPath></defs>
        {valid && <><polygon points={points.map(([x, y]) => `${px(x)},${py(y)}`).join(' ')} className="bed-2d-plate" />{texture && <image href={bedAsset(texture)} x={px(Math.min(...xs))} y={py(Math.max(...ys))} width={(Math.max(...xs) - Math.min(...xs)) * scale} height={(Math.max(...ys) - Math.min(...ys)) * scale} preserveAspectRatio="none" clipPath="url(#bed-clip)" />}<rect width="300" height="300" fill="url(#bed-grid)" clipPath="url(#bed-clip)" /><path d={`M ${px(0)} ${py(0)} h 24 M ${px(0)} ${py(0)} v -24`} className="bed-2d-axes" /><circle cx={px(0)} cy={py(0)} r="3" fill="#00ae42" /><text x={px(0) + 5} y={py(0) + 15} fontSize="11" className="bed-2d-label">0,0</text></>}
      </svg></ModalBody>
      <ModalFooter><button className="btn" type="button" onClick={close}>Cancel</button><button className="btn-primary" type="submit" disabled={!valid}>Save</button></ModalFooter>
    </form>
  </Modal>;
}
