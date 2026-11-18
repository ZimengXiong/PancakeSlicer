import { Dropdown } from './Dropdown';
import { useState } from 'react';
import { OPTIONS } from '../options';
import { getState, useStore } from '../state/store';
import { plateLabel, plateSetup } from '../profiles/plate-setup';
import { filamentColor } from '../plater/filamentColor';
import { setPlateSettings } from '../plater/plates';
import { Modal, ModalBody, ModalFooter, ModalTitle } from './Modal';
import { slotFilaments } from '../profiles/slots';

type Range = { from: number; to: number; order: number[] };
const ints = (v: unknown) => String(v ?? '').split(/[,;]/).map((x) => Number(x.trim())).filter((x) => Number.isFinite(x));

function ranges(v: unknown, nums: unknown, n: number): Range[] {
  const flat = ints(v), count = Number(nums) || 0, out: Range[] = [];
  for (let k = 0, i = 0; k < count && i + 2 + n <= flat.length; k++, i += 2 + n) out.push({ from: flat[i], to: flat[i + 1], order: flat.slice(i + 2, i + 2 + n) });
  return out;
}

function Order({ order, colours, onChange }: { order: number[]; colours: string[]; onChange(o: number[]): void }) {
  const move = (i: number, d: number) => { const o = [...order]; [o[i], o[i + d]] = [o[i + d], o[i]]; onChange(o); };
  return (
    <span className="plate-order">
      {order.map((f, i) => (
        <span key={f} className="plate-order-item">
          <span className="menu-swatch" style={{ background: colours[f - 1] }} />{f}
          <button type="button" className="btn small" disabled={i === 0} aria-label="Move up" onClick={() => move(i, -1)}>‹</button>
          <button type="button" className="btn small" disabled={i === order.length - 1} aria-label="Move down" onClick={() => move(i, 1)}>›</button>
        </span>
      ))}
    </span>
  );
}

export default function PlateSettings({ close }: { close(): void }) {
  const plate = useStore((s) => s.projectPlates.find((p) => p.index === s.plateIndex));
  const s = getState();
  const n = slotFilaments(s).length, colours = slotFilaments(s).map((_, i) => filamentColor(s, i + 1));
  const natural = Array.from({ length: n }, (_, i) => i + 1);
  const cfg = plate?.settings ?? {};
  const [name, setName] = useState(plate?.name ?? '');
  const [seq, setSeq] = useState(String(cfg.print_sequence ?? ''));
  const [spiral, setSpiral] = useState(cfg.spiral_mode === undefined ? '' : ['1', 'true'].includes(String(cfg.spiral_mode)) ? '1' : '0');
  const first0 = ints(cfg.first_layer_print_sequence).filter((f) => f >= 1 && f <= n);
  const [first, setFirst] = useState(first0.length === n ? first0 : null);
  const [others, setOthers] = useState<Range[] | null>(() => { const r = ranges(cfg.other_layers_print_sequence, cfg.other_layers_print_sequence_nums, n); return r.length ? r : null; });
  const seqDef = OPTIONS.print_sequence;
  const setup = useStore(plateSetup);
  const ok = () => {
    const out: Record<string, string> = {};
    if (seq) out.print_sequence = seq;
    if (spiral) out.spiral_mode = spiral;
    if (first) out.first_layer_print_sequence = first.join(',');
    if (others?.length) {
      out.other_layers_print_sequence = others.flatMap((r) => [r.from, r.to, ...r.order]).join(',');
      out.other_layers_print_sequence_nums = String(others.length);
    }
    setPlateSettings(name, out);
    close();
  };
  const seg = (label: string, value: string, set: (v: string) => void, options: [string, string][]) => <div className="form-row"><span>{label}</span>
    <Dropdown className="combo" aria-label={label} value={value} onValueChange={set} options={options.map(([v, text]) => ({ value: v, label: text }))} /></div>;
  return (
    <Modal size="m" label="Plate settings" onClose={close}>
      <ModalTitle title="Plate settings" onClose={close} />
      <ModalBody className="modal-fields">
        <label className="form-row"><span>Name</span><input className="text" value={name} onChange={(e) => setName(e.target.value)} /></label>
        {setup?.plate && <div className="form-row"><span>Build plate</span><span className="plate-printer-value">{plateLabel(setup, setup.plate)}</span></div>}
        <label className="form-row"><span>Print sequence</span>
          <Dropdown className="combo" value={seq} onValueChange={setSeq} options={[{ value: "", label: 'Project default' }, ...(seqDef?.enum_values?.map((v, i) => ({ value: v, label: seqDef.enum_labels?.[i] ?? v })) ?? [])]} />
        </label>
        {seg('Spiral vase', spiral, setSpiral, [['', 'Default'], ['1', 'On'], ['0', 'Off']])}
        {n > 1 && <>
          {seg('First layer order', first ? 'c' : 'a', (next) => setFirst(next === 'c' ? natural : null), [['a', 'Auto'], ['c', 'Custom']])}
          {first && <Order order={first} colours={colours} onChange={setFirst} />}
          {seg('Other layers order', others ? 'c' : 'a', (next) => setOthers(next === 'c' ? [{ from: 2, to: 2, order: natural }] : null), [['a', 'Auto'], ['c', 'Custom']])}
          {others?.map((r, k) => (
            <div key={k} className="plate-range">
              <span>Layers</span>
              <input className="text num" type="number" min={2} aria-label="From layer" value={r.from} onChange={(e) => setOthers(others.map((x, j) => (j === k ? { ...x, from: Math.max(2, Number(e.target.value)) } : x)))} />
              <span>to</span>
              <input className="text num" type="number" min={r.from} aria-label="To layer" value={r.to} onChange={(e) => setOthers(others.map((x, j) => (j === k ? { ...x, to: Math.max(x.from, Number(e.target.value)) } : x)))} />
              <Order order={r.order} colours={colours} onChange={(o) => setOthers(others.map((x, j) => (j === k ? { ...x, order: o } : x)))} />
              <button type="button" className="btn-ghost" aria-label={`Remove layers ${r.from}–${r.to}`} onClick={() => setOthers(others.length > 1 ? others.filter((_, j) => j !== k) : null)}>Remove</button>
            </div>
          ))}
          {others && <button type="button" className="btn plate-add" onClick={() => { const last = others[others.length - 1]; setOthers([...others, { from: last.to + 1, to: last.to + 1, order: natural }]); }}>Add layer range</button>}
        </>}
      </ModalBody>
      <ModalFooter>
        <button type="button" className="btn" onClick={close}>Cancel</button>
        <button type="button" className="btn-primary" onClick={ok}>Save</button>
      </ModalFooter>
    </Modal>
  );
}
