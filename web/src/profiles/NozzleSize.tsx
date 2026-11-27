import { Dropdown, type DropdownOption } from '../components/Dropdown';
import { toast, useStore } from '../state/store';
import { useMachine } from '../flow/fsm';
import { nozzleSize } from '../flow/nozzle-size';
import { lineRead, useCatalogRead } from '../catalog/read';
import { nozzlePresets, switchNozzle } from './nozzle-switch';
import type { CatalogPrinter } from '../catalog/types';

const OTHER = 'other';
const mm = (n: number) => `${n} mm`;

export function NozzleSize({ printerId, disabled, grid, label }: { printerId: string; disabled?: boolean; grid?: boolean; label?: string }) {
  const model = useStore((s) => s.records.printers.find((p) => p.id === printerId && !p.archived)?.model);
  return model ? <NozzleSizes printerId={printerId} model={model} disabled={disabled} grid={grid} label={label}/> : null;
}
function NozzleSizes({ printerId, model, disabled, grid, label }: { printerId: string; model: string; disabled?: boolean; grid?: boolean; label?: string }) {
  const printer = useStore((s) => s.records.printers.find((p) => p.id === printerId));
  const line = useCatalogRead(lineRead('printers', model)).value as CatalogPrinter | null;
  if (!printer || !line) return null;
  const n = printer.nozzles[0];
  const sizes = new Map<number, string>();
  for (const v of line.variants) if (v.default) sizes.set(Number(v.nozzle), '');
  for (const p of [...nozzlePresets(printer, line), { diameter: n.diameter, variants: printer.nozzles.map((x) => x.variant) }]) {
    const bases = [...new Set(p.variants.flatMap((id) => line.variants.filter((v) => v.id === id).map((v) => Number(v.nozzle))))];
    if (p.diameter === n.diameter || !sizes.has(p.diameter))
      sizes.set(p.diameter, bases.length && bases.some((b) => b !== p.diameter) ? `based on ${bases.join(' / ')} mm` : '');
  }
  const options = [...sizes].sort(([a], [b]) => a - b).map(([value, sub]) => ({ value, label: mm(value), sub }));
  return <div className={grid ? 'nozzle-sizes grid' : 'nozzle-sizes'}>
    <NozzleRow printerId={printerId} label={label ?? (printer.nozzles.length === 1 ? 'Nozzle' : 'Nozzles')} diameter={n.diameter}
      options={[...options, { value: OTHER, label: 'Other size…' }]} disabled={disabled} grid={grid}/>
  </div>;
}

function NozzleRow({ printerId, label, diameter, options, disabled, grid }: { printerId: string; label: string; diameter: number; options: DropdownOption[]; disabled?: boolean; grid?: boolean }) {
  const m = nozzleSize(printerId), snap = useMachine(m), saving = snap.value === 'saving';
  const run = (size: number) => {
    m.send({ type: 'SAVE' });
    const request = m.get().context.request;
    switchNozzle(printerId, size).then(() => m.send({ type: 'SAVED', request }), (e) => { const error = e instanceof Error ? e.message : String(e); m.send({ type: 'FAIL', request, error }); toast('nozzlesize_failed', { detail: error }); });
  };
  const pick = (value: string) => {
    if (value === OTHER) { m.send({ type: 'OTHER', text: String(diameter) }); return; }
    if (Number(value) !== diameter) run(Number(value));
  };
  const typed = Number(snap.context.text), valid = typed >= 0.1 && typed <= 2;
  const control = snap.value === 'entering'
    ? <span className="nozzle-other">
        <input className="text" type="number" min={0.1} max={2} step="any" aria-label={`${label} diameter in mm`} value={snap.context.text} autoFocus
          onChange={(e) => m.send({ type: 'TYPE', text: e.target.value })}
          onKeyDown={(e) => { if (e.key === 'Enter' && valid) run(typed); else if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); m.send({ type: 'CANCEL' }); } }}/>
        <span className="opt-unit">mm</span>
        <button type="button" className="btn primary" disabled={!valid} onClick={() => run(typed)}>Apply</button>
        <button type="button" className="btn" onClick={() => m.send({ type: 'CANCEL' })}>Cancel</button>
      </span>
    : <Dropdown className="combo two-line" aria-label={`${label} size`} disabled={disabled || saving} value={String(diameter)} options={options} onValueChange={pick}/>;
  return <div className={grid ? 'opt-line' : 'nozzle-row'}><span className="opt-label">{label}</span><span className="opt-field">{control}</span></div>;
}
