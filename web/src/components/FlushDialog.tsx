import { useState } from 'react';
import { Dropdown } from './Dropdown';
import { printerPreset } from '../state/store';
import { extruderLabel } from '../multinozzle/config';
import { nozzleCount } from '../profiles/view';
import { allFlush, MAX_FLUSH_VOLUME, MULTIPLIER_RANGE, nozzleFlush } from '../flush-state';
import { setState, useStore } from '../state/store';
import { Modal, ModalBody, ModalFooter, ModalTitle } from './Modal';
import { SlotChip } from './SlotChip';
import { filamentColor } from '../plater/filamentColor';
import { slotFilaments } from '../profiles/slots';
import { setFlushing } from '../profiles/plate-setup';

const TYPICAL_MAX_FLUSH_VOLUME = 900;

const filledSlots = (s: import('../state/store').State) => slotFilaments(s).flatMap((f, i) => f ? [i] : []);

export function FlushDialog() {
  const open = useStore((s) => s.flushDialog);
  const answered = useStore((s) => !!allFlush(s));
  const shape = useStore((s) => { const f = allFlush(s), n = filledSlots(s).length, nozzles = nozzleCount(s);
    return f && f.multipliers.length === nozzles && f.matrix.length === nozzles * n * n ? `${n}x${nozzles}` : ''; });
  if (!open || !answered || !shape) return null;
  return <FlushTable key={shape} />;
}

function FlushTable() {
  const s = useStore((st) => st);
  const [nozzle, setNozzle] = useState(0);
  const count = nozzleCount(s);
  const slots = filledSlots(s), colours = slots.map((i) => filamentColor(s, i + 1));
  const n = colours.length;
  const init = allFlush(s)!, record = s.records.printers.find((p) => p.id === s.printer);
  const [raw, setRaw] = useState<number[]>(init.matrix);
  const [multipliers, setMultipliers] = useState(init.multipliers);
  const [multTexts, setMultTexts] = useState(init.multipliers.map(String));
  const mult = multipliers[nozzle], multText = multTexts[nozzle];
  const auto = nozzleFlush(s, nozzle)?.auto ?? [];
  const offset = nozzle * n * n;
  const minVol = nozzleFlush(s, nozzle)?.minVolume ?? 0;
  const close = () => setState({ flushDialog: false });

  const shown = (i: number) => Math.min(MAX_FLUSH_VOLUME, Math.max(0, Math.round(raw[offset + i] * mult)));
  const onCell = (i: number, text: string) => {
    let v = parseInt(text, 10);
    if (!Number.isFinite(v) || v < 0) v = 0;
    v = Math.min(v, MAX_FLUSH_VOLUME);
    const next = [...raw];
    next[offset + i] = mult === 0 ? 0 : Math.round(v / mult);
    setRaw(next);
  };
  const onMult = (text: string) => {
    setMultTexts((texts) => texts.map((old, i) => i === nozzle ? text : old));
    let v = parseFloat(text);
    if (!Number.isFinite(v)) v = 1;
    v = Math.min(MULTIPLIER_RANGE[1], Math.max(MULTIPLIER_RANGE[0], v));
    setMultipliers((values) => values.map((old, i) => i === nozzle ? v : old));
  };
  const save = () => {
    const matrix = raw.map((v, i) => Math.min(Math.max(v, 0), multipliers[Math.floor(i / (n * n))] > 0 ? MAX_FLUSH_VOLUME / multipliers[Math.floor(i / (n * n))] : MAX_FLUSH_VOLUME));
    const autoAll = Array.from({ length: count }, (_, i) => nozzleFlush(s, i)?.auto ?? []).flat();
    const volumes = matrix.map((v, i) => v === autoAll[i] ? null : v);
    const keepMultipliers = !!record?.flushing?.multipliers?.length || multipliers.some((v, i) => v !== init.multipliers[i]);
    void setFlushing({ ...(volumes.some((v) => v !== null) ? { volumes } : {}), ...(keepMultipliers ? { multipliers } : {}) });
    setState({ flushDialog: false });
  };
  const outOfRange = raw.slice(offset, offset + n * n).some((_, i) => Math.floor(i / n) !== i % n && (shown(i) < minVol || shown(i) > MAX_FLUSH_VOLUME));

  return (
    <Modal size={n > 6 ? 'l' : 'm'} label="Flushing volumes" onClose={close}>
        <ModalTitle title="Flushing volumes" onClose={close} />
        <ModalBody>
          {count > 1 && <label className="form-row"><span>Nozzle</span><Dropdown className="combo" aria-label="Flushing nozzle" value={String(nozzle)} onValueChange={(value) => setNozzle(Number(value))}
            options={Array.from({ length: count }, (_, i) => ({ value: String(i), label: extruderLabel(i, count, printerPreset(s)?.vendor === 'BBL') }))} /></label>}
          <label className="form-row"><span>Multiplier</span>
            <input type="number" step="0.1" min={MULTIPLIER_RANGE[0]} max={MULTIPLIER_RANGE[1]} className={`text num multiplier-input ${mult !== 1 ? 'changed' : ''}`}
              value={multText} onChange={(e) => onMult(e.target.value)} /></label>
          <div className="flush-scroll">
            <table className="flush-table">
              <thead>
                <tr>
                  <th>
                    <span className="flush-corner" aria-label="From / to" />
                  </th>
                  {colours.map((c, j) => (
                    <th key={j} aria-label={`To filament ${slots[j] + 1}`}><SlotChip n={slots[j] + 1} colour={c} /></th>
                  ))}
                </tr>
              </thead>
              <tbody>
                {colours.map((c, i) => (
                  <tr key={i}>
                    <th aria-label={`From filament ${slots[i] + 1}`}><SlotChip n={slots[i] + 1} colour={c} /></th>
                    {colours.map((_, j) => {
                      const k = i * n + j;
                      const v = i === j ? 0 : shown(k);
                      const def = Math.round(auto[k] * mult);
                      const cls = i === j ? '' : v < minVol || v > MAX_FLUSH_VOLUME ? 'bad' : v !== def ? 'changed' : '';
                      return (
                        <td key={j}>
                          <input
                            type="number"
                            className={`table-input ${cls}`}
                            value={v}
                            readOnly={i === j}
                            onChange={(e) => onCell(k, e.target.value)}
                            aria-label={`from ${slots[i] + 1} to ${slots[j] + 1}`}
                          />
                        </td>
                      );
                    })}
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
          <p className={outOfRange ? 'modal-error' : 'flush-note'} role={outOfRange ? 'alert' : undefined}>mm³ flushed when switching from row to column, typically {minVol}–{TYPICAL_MAX_FLUSH_VOLUME}</p>
        </ModalBody>
        <ModalFooter lead={<button type="button" className="btn-ghost" onClick={() => setRaw((values) => values.map((old, i) => i >= offset && i < offset + n * n ? auto[i - offset] : old))}>Recalculate</button>}>
          <button type="button" className="btn" onClick={close}>Cancel</button>
          <button type="button" className="btn-primary" onClick={save}>Save</button>
        </ModalFooter>
    </Modal>
  );
}
