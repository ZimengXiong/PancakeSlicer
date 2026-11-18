import { Range } from './Range';
import { Dropdown } from './Dropdown';
import { useState } from 'react';
import { getState } from '../state/store';
import { simplifyModel, triangleCount } from '../plater/meshOps';
import { Modal, ModalBody, ModalFooter, ModalTitle } from './Modal';

const LEVELS: [string, number][] = [['Extra high', 0.001], ['High', 0.01], ['Medium', 0.1], ['Low', 0.5], ['Extra low', 1]];

export default function SimplifyDialog({ close }: { close(): void }) {
  const s = getState(), o = s.objects.find((x) => x.id === s.selected?.object);
  const [useCount, setUseCount] = useState(false);
  const [level, setLevel] = useState(2);
  const [ratio, setRatio] = useState(50);
  if (!o) return null;
  const total = triangleCount(o), wanted = Math.max(1, Math.round(total * (1 - ratio / 100)));
  const count = useCount;
  return (
    <Modal size="m" label={`Simplify ${o.name}`} onClose={close}>
      <ModalTitle title={`Simplify ${o.name}`} onClose={close} />
      <ModalBody className="modal-fields">
        {<div className="form-row"><span>Method</span><Dropdown className="combo" aria-label="Method" value={count ? 'count' : 'level'} onValueChange={(next) => setUseCount(next === 'count')} options={[{ value: 'level', label: 'Detail level' }, { value: 'count', label: 'Reduce by' }]} /></div>}
        {!count ? <label className="form-row"><span>Detail level</span>
          <Dropdown className="combo" value={level} aria-label="Detail level" onValueChange={(next) => setLevel(Number(next))} options={[...LEVELS.map(([label], i) => ({ value: i, label }))]} /></label>
          : <label className="form-row"><span>Reduce by</span><span className="modal-unit simplify-ratio">
            <Range min={0} max={99} step={1} value={ratio} aria-label="Reduce by" onValueChange={(next) => setRatio(next)} />
            <input className="text num" type="number" min={0} max={99} step={0.5} value={ratio} aria-label="Reduce by, percent" onChange={(e) => setRatio(Math.min(99, Math.max(0, Number(e.target.value) || 0)))} /><span>%</span></span></label>}
        <div className="form-row"><span>Triangles</span><span>{total.toLocaleString()}{count && <> → {wanted.toLocaleString()}</>}</span></div>
      </ModalBody>
      <ModalFooter>
        <button type="button" className="btn" onClick={close}>Cancel</button>
        <button type="button" className="btn-primary" disabled={count && ratio === 0} onClick={() => { close(); void simplifyModel(o.id, wanted, count ? undefined : LEVELS[level][1]); }}>Simplify</button>
      </ModalFooter>
    </Modal>
  );
}
