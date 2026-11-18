import { Check } from './Check';
import { useEffect, useState } from 'react';
import { loadHead, loadHeadJson, maxSlope, saveUserHead, slopeMap, type Head, type HeadBox } from '../heads';
import { toast } from '../state/store';
import { Modal, ModalBody, ModalFooter, ModalTitle } from './Modal';

const decimal = (n: number) => { const r = Math.round(n * 1e6) / 1e6; return String(r === 0 ? 0 : r); };
function number(text: string) {
  const v = Number(text.trim());
  if (text.trim() === '' || !Number.isFinite(v)) throw new Error('Please enter a valid number.');
  return v;
}

type Row = { name: string; v: string[]; ix: boolean; iy: boolean; nozzle: boolean };
const toRow = (b: HeadBox): Row => ({
  name: b.name ?? '', ix: !!b.infinite_x, iy: !!b.infinite_y, nozzle: !!b.nozzle,
  v: [b.min[2], b.max[2], (b.min[0] + b.max[0]) / 2, (b.min[1] + b.max[1]) / 2, (b.max[0] - b.min[0]) / 2, (b.max[1] - b.min[1]) / 2].map(decimal),
});
function toBox(r: Row): HeadBox {
  const [z0, z1, cx, cy, hx, hy] = r.v.map(number);
  if (hx < 0 || hy < 0 || z1 < z0) throw new Error('Half-widths can\'t be negative, and Z max needs to be at least Z min.');
  return { name: r.name, min: [cx - hx, cy - hy, z0], max: [cx + hx, cy + hy, z1], ...(r.ix && { infinite_x: true }), ...(r.iy && { infinite_y: true }), ...(r.nozzle && { nozzle: true }) };
}
const COLUMNS = ['Component', 'Z min (mm)', 'Z max (mm)', 'Center X (mm)', 'Center Y (mm)', 'Half X (mm)', 'Half Y (mm)', 'Infinite X', 'Infinite Y', 'Nozzle'];

const footprint = (b: HeadBox, lateral: number) => {
  const grow = b.nozzle ? 0 : lateral;
  return [b.min[0] - grow, b.min[1] - grow, b.max[0] + grow, b.max[1] + grow];
};
const projection = (b: HeadBox, grow: number, ux: number, uy: number) => {
  const [lx, ly, hx, hy] = footprint(b, grow);
  return (ux >= 0 ? hx : lx) * ux + (uy >= 0 ? hy : ly) * uy;
};

function fromSlopeStar(base: Head, samples: { azimuth: number; slope: number }[], gap: number, date: string): Head {
  for (const s of samples)
    if (!Number.isFinite(s.slope) || s.slope < 0 || s.slope >= Math.PI / 2) throw new Error('Clean slope angles need to be between 0° and 90°.');
  if (!(gap > 0) || !base.boxes.length) throw new Error('Pick a valid base print head and a layer height above 0 first.');
  const out: Head = { ...base, boxes: base.boxes.map((b) => ({ ...b, min: [...b.min] as HeadBox['min'], max: [...b.max] as HeadBox['max'] })) };
  const finite = out.boxes.filter((b) => !b.infinite_x && !b.infinite_y);
  for (const box of finite) {
    let floor = box.min[2];
    for (const s of samples) {
      const p = projection(box, out.lateral_margin, Math.cos(s.azimuth), Math.sin(s.azimuth));
      if (p > 0) floor = Math.max(floor, Math.tan(s.slope) * p - gap + (box.nozzle ? 0 : out.margin));
    }
    box.min[2] = floor;
    box.max[2] = Math.max(box.max[2], floor);
  }
  for (const s of samples) {
    if (maxSlope(out, s.azimuth, gap) <= s.slope) continue;
    const ux = Math.cos(s.azimuth), uy = Math.sin(s.azimuth);
    let binding: HeadBox | null = null, best = Infinity, proj = 0;
    for (const box of finite) {
      const p = projection(box, out.lateral_margin, ux, uy);
      if (p <= 0) continue;
      const limit = (gap + box.min[2] - (box.nozzle ? 0 : out.margin)) / p;
      if (limit < best) { best = limit; binding = box; proj = p; }
    }
    if (!binding) throw new Error('The selected base print head is missing clearance geometry for one of the directions.');
    binding.min[2] = Math.tan(s.slope) * proj - gap + (binding.nozzle ? 0 : out.margin) - 1e-10;
  }
  return { ...out, calibrated: true, provenance: `slope star ${date}, base ${base.id}` };
}

function SlopePlot({ head, gap }: { head: Head | null; gap: number }) {
  const size = 300, c = size / 2, radius = c - 30;
  const slopes = head && gap > 0 ? slopeMap(head, 72, gap) : null;
  const points = slopes?.map((s, i) => {
    const phi = (2 * Math.PI * i) / slopes.length, r = (radius * s) / (Math.PI / 2);
    return `${(c + r * Math.cos(phi)).toFixed(1)},${(c - r * Math.sin(phi)).toFixed(1)}`;
  }).join(' ');
  return (
    <figure className="head-plot">
      <svg viewBox={`0 0 ${size} ${size}`} role="img" aria-label="Clean slope limit by direction">
        {[30, 60, 90].map((a) => <g key={a}><circle cx={c} cy={c} r={(radius * a) / 90} fill="none" className="plot-grid" />
          <text x={c + (radius * a) / 90 + 2} y={c - 2} fontSize="11" fill="currentColor">{a}°</text></g>)}
        <path d={`M ${c - radius} ${c} H ${c + radius} M ${c} ${c - radius} V ${c + radius}`} className="plot-grid" />
        <text x={c + radius - 4} y={c - 16} fontSize="11" fill="currentColor">+X</text>
        <text x={c - 8} y={c - radius - 6} fontSize="11" fill="currentColor">+Y</text>
        {points && <polygon points={points} style={{ fill: 'color-mix(in srgb, var(--accent) 18%, transparent)', stroke: 'var(--accent-text)' }} strokeWidth="1.5" />}
        {!slopes && <text x={6} y={16} fontSize="11" fill="currentColor">Pick a valid profile and layer height to preview your slope limits.</text>}
      </svg>
      {slopes && <figcaption>Min {((Math.min(...slopes) * 180) / Math.PI).toFixed(1)}°</figcaption>}
    </figure>
  );
}

const SLOPE_STAR_TEXT = `For a head that reaches further on one side. Most printers only need Calibration > Non-planar > Head clearance.
1. Check the head profile and margins on the Head profile tab.
2. Print resources/calib/nonplanar/slope_star.stl at the layer height shown there. Each of its eight wedges climbs in steps of rising slope toward one direction; thin pillars at the head's reach show scuffing.
3. For each direction, counterclockwise from +X, enter the last angle that printed clean, or 0° if none did.
Apply fits the head to these angles; a component shared by several directions can lower the limit in the others. Save it under a new head ID to keep it.`;

export default function HeadDialog({ id, layerHeight, close, onSaved }: { id: string; layerHeight: number; close: () => void; onSaved: (id: string) => void }) {
  const [load, setLoad] = useState<{ state: 'loading' | 'missing' } | { state: 'failed'; error: string } | { state: 'ready' | 'saving'; head: Head; json: Record<string, unknown> }>({ state: 'loading' });
  const base = 'head' in load ? { head: load.head, json: load.json } : load.state === 'loading' ? undefined : null;
  const [rows, setRows] = useState<Row[]>([]);
  const [selected, setSelected] = useState(-1);
  const [fields, setFields] = useState({ id: '', name: '', margin: '', lateral: '', gap: decimal(layerHeight > 0 ? layerHeight : 0.2) });
  const [calibrated, setCalibrated] = useState(false);
  const [provenance, setProvenance] = useState('');
  const [page, setPage] = useState(0);
  const [angles, setAngles] = useState<string[]>(Array(8).fill(''));
  useEffect(() => {
    const key = id || 'generic-conservative';
    let live = true;
    setLoad({ state: 'loading' });
    Promise.all([loadHead(key), loadHeadJson(key)]).then(([head, json]) => {
      if (!live) return;
      if (!head || !json) return setLoad({ state: 'missing' });
      setLoad({ state: 'ready', head, json });
      setRows(head.boxes.map(toRow));
      setFields((f) => ({ ...f, id: `${head.id}-user`, name: head.name, margin: decimal(head.margin), lateral: decimal(head.lateral_margin) }));
      setCalibrated(head.calibrated);
      setProvenance(head.provenance ?? '');
    }, (e: unknown) => { if (live) setLoad({ state: 'failed', error: e instanceof Error ? e.message : String(e) }); });
    return () => { live = false; };
  }, [id]);
  const field = (k: keyof typeof fields) => (e: React.ChangeEvent<HTMLInputElement>) => {
    setFields({ ...fields, [k]: e.target.value });
    if (k === 'margin' || k === 'lateral') setCalibrated(false);
  };
  const edit = (next: Row[]) => { setRows(next); setCalibrated(false); };
  const readHead = (): Head => {
    const margin = number(fields.margin), lateral = number(fields.lateral);
    if (margin < 0 || lateral < 0) throw new Error('Clearance margins can\'t be negative.');
    return { ...base!.head, id: fields.id, name: fields.name, margin, lateral_margin: lateral, calibrated, provenance, boxes: rows.map(toBox) };
  };
  const readGap = () => { const g = number(fields.gap); if (g <= 0) throw new Error('Layer height needs to be greater than 0.'); return g; };
  let preview: Head | null = null, gap = 0;
  try { if (base) { preview = readHead(); gap = readGap(); } } catch { preview = null; }
  const apply = () => {
    try {
      const date = new Date().toISOString().slice(0, 10);
      const samples = angles.map((a, i) => ({ azimuth: (i * Math.PI) / 4, slope: (number(a) * Math.PI) / 180 }));
      const fitted = fromSlopeStar({ ...readHead(), id: base!.head.id }, samples, readGap(), date);
      setRows(fitted.boxes.map(toRow));
      setCalibrated(true);
      setProvenance(fitted.provenance ?? '');
      setPage(0);
    } catch (e) {
      toast('couldnt_calibrate_print_head', { detail: e instanceof Error ? e.message : String(e) });
    }
  };
  const save = async () => {
    if (load.state !== 'ready') return;
    setLoad({ ...load, state: 'saving' });
    try {
      const head = readHead();
      if (!/^[A-Za-z0-9_-]+$/.test(head.id)) throw new Error('Print head IDs can only use letters, numbers, hyphens (-), and underscores (_).');
      const json: Record<string, unknown> = { ...base!.json, id: head.id, name: head.name, calibrated: head.calibrated, margin: head.margin, lateral_margin: head.lateral_margin, boxes: head.boxes };
      if (head.provenance) json.provenance = head.provenance;
      delete json.side_profile; delete json.side_profile_top; delete json.sides;
      await saveUserHead(json);
      onSaved(head.id);
      close();
    } catch (e) {
      toast('couldnt_save_print_head', { detail: e instanceof Error ? e.message : String(e) });
      setLoad(load);
    }
  };
  return (
    <Modal size="xl" label="Print head" onClose={close}>
      <ModalTitle title="Print head" onClose={close} />
      <ModalBody>
        {base === undefined && <p role="status">Loading head…</p>}
        {load.state === 'missing' && <p role="alert">“{id}” isn't in the print head library—using the safe generic print head instead.</p>}
        {load.state === 'failed' && <p role="alert">Oops! Couldn't load that print head: {load.error}</p>}
        {base && <>
          <div className="head-identity">
            <label className="form-row"><span>ID</span><input className="text" value={fields.id} onChange={field('id')} /></label>
            <label className="form-row"><span>Name</span><input className="text" value={fields.name} onChange={field('name')} /></label>
          </div>
          <span className="seg head-tabs" role="tablist" aria-label="Head">
            {['Components', 'Per-direction fit'].map((p, i) => <button key={p} type="button" role="tab" aria-selected={i === page} aria-checked={i === page} onClick={() => setPage(i)}>{p}</button>)}
          </span>
          {page === 0 && <>
            <div className="head-boxes">
              <table className="np-table head-grid">
                <thead><tr>{COLUMNS.map((h) => <th key={h}>{h}</th>)}</tr></thead>
                <tbody>{rows.map((r, i) => (
                  <tr key={i} className={i === selected ? 'selected' : ''} onFocus={() => setSelected(i)} onClick={() => setSelected(i)}>
                    <td><input className="cell" aria-label="Component" value={r.name} onChange={(e) => edit(rows.map((x, j) => (j === i ? { ...x, name: e.target.value } : x)))} /></td>
                    {r.v.map((v, k) => <td key={k}><input className="cell num" aria-label={COLUMNS[k + 1]} value={v}
                      onChange={(e) => edit(rows.map((x, j) => (j === i ? { ...x, v: x.v.map((y, m) => (m === k ? e.target.value : y)) } : x)))} /></td>)}
                    {(['ix', 'iy', 'nozzle'] as const).map((k) => <td key={k}><Check aria-label={k} checked={r[k]}
                      onCheckedChange={(next) => edit(rows.map((x, j) => (j === i ? { ...x, [k]: next } : x)))} /></td>)}
                  </tr>
                ))}</tbody>
              </table>
            </div>
            <div className="form-actions">
              <button type="button" className="btn" onClick={() => edit([...rows, { name: 'New component', v: ['0', '10', '0', '0', '0', '0'], ix: false, iy: false, nozzle: false }])}>Add component</button>
              <button type="button" className="btn-ghost" disabled={selected < 0 || selected >= rows.length} onClick={() => { edit(rows.filter((_, j) => j !== selected)); setSelected(-1); }}>Remove component</button>
            </div>
            <div className="modal-split">
              <div className="modal-fields">
                <label className="form-row"><span>Vertical margin</span><span className="modal-unit"><input id="head-margin" className="text num" value={fields.margin} onChange={field('margin')} /><span>mm</span></span></label>
                <label className="form-row"><span>Lateral margin</span><span className="modal-unit"><input id="head-lateral" className="text num" value={fields.lateral} onChange={field('lateral')} /><span>mm</span></span></label>
                <label className="form-row"><span>Layer height</span><span className="modal-unit"><input id="head-gap" className="text num" value={fields.gap} onChange={field('gap')} /><span>mm</span></span></label>
              </div>
              <SlopePlot head={preview} gap={gap} />
            </div>
          </>}
          {page === 1 && <>
            <p className="ask-message">{SLOPE_STAR_TEXT}</p>
            <table className="np-table head-angles">
              <thead><tr><th>Azimuth</th><th>Last clean angle</th></tr></thead>
              <tbody>{angles.map((a, i) => <tr key={i}><td className="num">{i * 45}°</td>
                <td><input className="cell num" aria-label={`Last clean angle at ${i * 45}°`} value={a} onChange={(e) => setAngles(angles.map((x, j) => (j === i ? e.target.value : x)))} /></td></tr>)}</tbody>
            </table>
            <div className="form-actions"><button type="button" className="btn" onClick={apply}>Apply to components</button></div>
          </>}
        </>}
      </ModalBody>
      <ModalFooter>
        <button type="button" className="btn" onClick={close}>Cancel</button>
        <button type="button" className="btn-primary" disabled={load.state !== 'ready'} onClick={() => void save()}>Save head</button>
      </ModalFooter>
    </Modal>
  );
}
