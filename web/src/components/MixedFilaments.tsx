import { Check } from './Check';
import { Range } from './Range';
import { Dropdown } from './Dropdown';
import { useState } from 'react';
import { ActionMenu } from './Menu';
import { getState, useStore, type Mix, type State } from '../state/store';
import { addMix, blend, broken, componentColour, deleteMixes, editMix, filamentType, physicalCount, recommendations, SLOTS_MAX } from '../plater/mixed';
import { filamentColor } from '../plater/filamentColor';
import { confirmDialog } from '../actions';
import { Modal, ModalBody, ModalFooter, ModalTitle } from './Modal';
import './MixedFilaments.css';
import { slotExtruder } from '../printers/feeds';
import { extruderLabel } from '../multinozzle/config';
import { slotFilaments, slotMixes } from '../profiles/slots';

const MIN_RATIO = 10;
const ramp = (s: State, m: Mix) => {
  const [a, b] = m.components.map((c) => componentColour(s, c));
  return `linear-gradient(to bottom, ${m.gradient ? b : a}, ${m.gradient ? a : b})`;
};
const swatch = (s: State, m: Mix, i: number) => (m.gradient !== null && m.components.length === 2 ? ramp(s, m) : filamentColor(s, i + 1));

export default function MixedFilaments({ close }: { close(): void }) {
  const mixes = useStore((s) => slotMixes(s));
  useStore((s) => s.records);
  useStore((s) => slotFilaments(s));
  const s = getState();
  const [dialog, setDialog] = useState<{ slot: number | null } | null>(null);
  const [menu, setMenu] = useState<{ slot: number; x: number; y: number } | null>(null);
  const slots = Object.keys(mixes).map(Number).sort((a, b) => a - b);
  const full = slotFilaments(s).length >= SLOTS_MAX;
  const removeAll = () => void confirmDialog('Mixed filament', `Remove all ${slots.length} mixed filaments?`).then((ok) => ok && deleteMixes(slots, undefined, 'Remove all mixed filaments'));
  return (
    <Modal size="m" label="Mixed filament" onClose={close}>
      <ModalTitle title="Mixed filament" onClose={close} />
      <ModalBody>
        {!slots.length ? <p className="mixed-empty">No mixed filaments</p> : <div className="mixed-list">
          {slots.map((i) => {
            const m = mixes[i], bad = broken(s, m);
            const parts = m.components.map((c, k) => `${c} (${m.ratios[k]}%)`).join(' + ');
            return (
              <div key={i} className={`mixed-row ${bad ? 'broken' : ''}`}>
                <button type="button" className="filament-swatch" aria-label={`Edit filament ${i + 1}`} style={{ background: swatch(s, m, i) }} onClick={() => setDialog({ slot: i })}>{i + 1}</button>
                <button type="button" className="mixed-content" aria-label={`Filament ${i + 1}: ${parts}`} onClick={() => setDialog({ slot: i })}>
                  {m.components.map((c, k) => (
                    <span key={k}>{k > 0 && (m.gradient !== null ? '→' : '+')}<i style={{ background: componentColour(s, c) }}>{c || '?'}</i></span>
                  ))}
                  {m.gradient === null && <em>{m.ratios.map((r) => `${r}%`).join(' ')}</em>}
                </button>
                <button type="button" className={`icon-btn mixed-more${bad ? ' broken' : ''}`} aria-label={bad ? 'Broken component references' : 'Edit, merge or delete'} title={bad ? 'Broken component references' : 'Edit, merge or delete'}
                  onClick={(e) => { const r = e.currentTarget.getBoundingClientRect(); setMenu(menu?.slot === i ? null : { slot: i, x: Math.min(r.left, window.innerWidth - 230), y: r.bottom }); }}>⋯</button>
              </div>
            );
          })}
        </div>}
      </ModalBody>
      <ModalFooter lead={slots.length > 1 && <button type="button" className="link" onClick={removeAll}>Remove all</button>}>
        <button type="button" className="btn" disabled={full} onClick={() => setDialog({ slot: null })}>Add mixed filament</button>
        <button type="button" className="btn-primary" onClick={close}>Done</button>
      </ModalFooter>
      {menu && <ActionMenu label={`Filament ${menu.slot + 1}`} at={menu} onClose={() => setMenu(null)} items={[
        { label: 'Edit', action: () => setDialog({ slot: menu.slot }) },
        { label: 'Merge with', sub: slotFilaments(s).flatMap((_, j) => j === menu.slot ? [] : [{ label: `Filament ${j + 1}`,
          icon: <i className="mixed-dot" style={{ background: filamentColor(s, j + 1) }} />,
          action: () => deleteMixes([menu.slot], j + 1, 'Merge filament') }]) },
        'sep', { label: 'Delete', action: () => deleteMixes([menu.slot]) },
      ]} />}
      {dialog && <MixDialog slot={dialog.slot} close={() => setDialog(null)} />}
    </Modal>
  );
}

function MixDialog({ slot, close }: { slot: number | null; close(): void }) {
  const s = getState();
  const n = physicalCount(s);
  const [m, setM] = useState<Mix>(() => (slot !== null && slotMixes(s)[slot]) || { components: [1, 2], ratios: [50, 50], gradient: null, perPart: false });
  const three = m.components.length === 3;
  const colours = m.components.map((c) => componentColour(s, c));
  const types = m.components.map((c) => (c >= 1 && c <= n ? filamentType(s, c) : ''));
  const unset = m.components.some((c) => c < 1 || c > n);
  const unknownType = !unset && types.some((t) => !t), mismatch = new Set(types.filter(Boolean)).size > 1;
  const printer = s.records.printers.find((p) => p.id === s.printer), count = printer?.nozzles.length ?? 1;
  const sides = m.components.map((c) => slotExtruder(printer, count, c - 1));
  const split = !unset && count > 1 && new Set(sides).size > 1;
  const strip = slotFilaments(s), same = !unset && new Set(m.components.map((c) => `${strip[c - 1]}|${colours[m.components.indexOf(c)]}`)).size === 1;
  const setRatio = (k: number, v: number) => {
    if (!three) return setM({ ...m, ratios: k ? [100 - v, v] : [v, 100 - v] });
    const r = [...m.ratios];
    r[k] = Math.max(MIN_RATIO, Math.min(100 - 2 * MIN_RATIO, v));
    const o = [0, 1, 2].filter((j) => j !== k), rest = 100 - r[k], had = r[o[0]] + r[o[1]] || 1;
    r[o[0]] = Math.max(MIN_RATIO, Math.min(rest - MIN_RATIO, Math.round((r[o[0]] * rest) / had)));
    r[o[1]] = rest - r[o[0]];
    setM({ ...m, ratios: r });
  };
  const preview = m.gradient !== null && !three
    ? `linear-gradient(to right, ${m.gradient ? colours[1] : colours[0]}, ${m.gradient ? colours[0] : colours[1]})`
    : `repeating-linear-gradient(to bottom, ${m.components.flatMap((_, k) => { const at = m.ratios.slice(0, k).reduce((a, b) => a + b, 0) / 10; return [`${colours[k]} ${at}px`, `${colours[k]} ${at + m.ratios[k] / 10}px`]; }).join(', ')})`;
  const ok = () => { if (slot === null) addMix(m); else editMix(slot, m); close(); };
  const recs = recommendations(s, three);
  return (
    <Modal size="m" label={slot === null ? 'Add mixed filament' : 'Edit mixed filament'} onClose={close} className="mixed-dialog">
      <ModalTitle title={slot === null ? 'Add mixed filament' : 'Edit mixed filament'} onClose={close} />
      <ModalBody className="modal-fields">
                <div className="mixed-preview" style={{ background: preview }}><i style={{ background: unset ? '#808080' : blend(colours, m.ratios) }} /></div>
        <h3 className="modal-group">Filaments</h3>
        {m.components.map((c, k) => (
          <label key={k} className="form-row"><span>Filament {k + 1}</span>
            <Dropdown className="combo" value={c} onValueChange={(next) => setM({ ...m, components: m.components.map((x, j) => (j === k ? Number(next) : x)) })} options={[...(!c ? [{ value: 0, label: 'Choose…' }] : []), ...Array.from({ length: n }, (_, j) => j + 1).filter((j) => j === c || !m.components.includes(j)).map((j) => (
                ({ value: j, label: <>{j}: {slotFilaments(s)[j - 1]}</> })
              ))]} />
          </label>
        ))}
        <div className="form-actions">
          {!three && <button type="button" className="btn-ghost" disabled={n < 3}
            onClick={() => setM({ ...m, components: [...m.components, 0], ratios: [33, 33, 34], gradient: null, perPart: false })}>Add a third filament</button>}
          {three && <button type="button" className="btn-ghost" onClick={() => setM({ ...m, components: m.components.slice(0, 2), ratios: [50, 50] })}>Remove the third filament</button>}
        </div>
        {(m.gradient === null || three) && <>
          <h3 className="modal-group">Ratio</h3>
          {three ? m.components.map((c, k) => (
            <label key={k} className="form-row"><span><i className="mixed-dot" style={{ background: colours[k] }} />{c || '?'}</span>
              <span className="modal-unit"><input className="text num" type="number" min={MIN_RATIO} max={100 - 2 * MIN_RATIO} value={m.ratios[k]} onChange={(e) => setRatio(k, Number(e.target.value))} /><span>%</span></span>
            </label>
          )) : (
            <label className="mixed-ratio">
              <span>{m.ratios[0]}%</span>
              <Range min={MIN_RATIO} max={100 - MIN_RATIO} step={1} value={m.ratios[1]} aria-label="Ratio"
                style={{ accentColor: colours[1] }} onValueChange={(next) => setRatio(1, next)} />
              <span>{m.ratios[1]}%</span>
            </label>
          )}
        </>}
        {!three && <>
          <label className="modal-check"><Check checked={m.gradient !== null} onCheckedChange={(next) => setM({ ...m, gradient: next ? 0 : null, perPart: false })} />Gradient</label>
          {m.gradient !== null && <label className="form-row"><span>Direction</span>
              <Dropdown className="combo" value={m.gradient} aria-label="Gradient direction" onValueChange={(next) => setM({ ...m, gradient: Number(next) as 0 | 1 })} options={[({ value: 0, label: <>{m.components[0]} → {m.components[1]}</> }), ({ value: 1, label: <>{m.components[1]} → {m.components[0]}</> })]} />
</label>}
          {m.gradient !== null && <label className="modal-check"><Check checked={m.perPart} onCheckedChange={(next) => setM({ ...m, perPart: next })} />Gradient across each part</label>}
        </>}
        {recs.length > 0 && <>
          <h3 className="modal-group">Suggestions</h3>
          <div className="mixed-recs">
            {recs.map((r, k) => (
              <button key={k} type="button" className="mixed-rec" aria-label={r.components.map((c, j) => `${c} (${r.ratios[j]}%)`).join(' + ')}
                style={{ background: blend(r.components.map((c) => componentColour(s, c)), r.ratios) }} onClick={() => setM(r)} />
            ))}
          </div>
        </>}
        {same && !mismatch && <p className="hint-text" role="note">Both slots have the same filament and colour, so the mix will print just like either one!</p>}
        {mismatch && <p className="modal-error" role="alert">Mix filaments of one type ({m.components.map((c, k) => `${c}: ${types[k]}`).join(', ')}).</p>}
        {!mismatch && unknownType && <p className="hint-text" role="status">Checking filament types…</p>}
        {split && <p className="modal-error" role="alert">Mix filaments on one nozzle ({m.components.map((c, k) => `${c}: ${sides[k] === undefined ? 'no nozzle' : extruderLabel(sides[k]!, count, printer?.vendor === 'BBL')}`).join(', ')}).</p>}
      </ModalBody>
      <ModalFooter>
        <button type="button" className="btn" onClick={close}>Cancel</button>
        <button type="button" className="btn-primary" disabled={unset || mismatch || unknownType || split} onClick={ok}>{slot === null ? 'Add' : 'Save'}</button>
      </ModalFooter>
    </Modal>
  );
}
