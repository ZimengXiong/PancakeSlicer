import { Dropdown } from '../components/Dropdown';
import { resolvePreset, toast, useStore } from '../state/store';
import { selectionValues } from './view';
import './extruder-rows.css';
import { extruderLabel, valuesOf } from '../multinozzle/config';
import { INSTALLED } from './records';
import { setPrinterNozzle } from './record-edits';

const TYPES = ['Direct Drive', 'Bowden'];
const split = (variant: string) => { const type = TYPES.find((t) => variant.startsWith(`${t} `)) ?? 'Direct Drive'; return { type, volume: variant.slice(type.length).trim() || 'Standard' }; };

export function ExtruderRows({ printerId, disabled, grid }: { printerId?: string; disabled?: boolean; grid?: boolean }) {
  const selected = useStore((s) => s.printer), id = printerId ?? selected;
  const printer = useStore((s) => s.records.printers.find((p) => p.id === id));
  const known = useStore((s) => id !== s.printer || selectionValues(s, 'machine', 0).state === 'ready');
  const lists = valuesOf(useStore((s) => resolvePreset(s, 'machine', id).config.extruder_variant_list));
  if (!printer) return null;
  const rows = printer.nozzles.map((n, i) => {
    const installed = `${n.type ?? INSTALLED.type} ${n.volume ?? INSTALLED.volume}`;
    return { installed, nozzle: String(n.diameter), options: [...new Set([...(lists[i] ?? '').split(',').map((v) => v.trim()).filter(Boolean), installed])] };
  });
  if (rows.length < 2 && (!known || !rows.some((r) => r.options.length > 1))) return null;
  const bambu = printer.vendor === 'BBL';
  const set = (i: number, variant: string) => void setPrinterNozzle(id, i, split(variant))
    .catch((error) => toast('extruderrows_failed', { detail: error instanceof Error ? error.message : String(error) }));
  return <div className={grid ? 'extruder-rows grid' : 'extruder-rows'} role="group" aria-label="Extruders">{rows.map((row, i) => {
    const label = rows.length > 1 ? extruderLabel(i, rows.length, bambu).replace(/ nozzle$/, '') : 'Extruder';
    const control = row.options.length > 1 ? <Dropdown className="combo two-line" aria-label={`${label} extruder`} disabled={disabled} value={row.installed}
      options={row.options.map((value) => ({ value, label: value, short: value.replace(/^Direct Drive /, ''), sub: `${row.nozzle} mm` }))} onValueChange={(value) => value !== row.installed && set(i, value)}/>
      : <span className="extruder-variant"><span className="dropdown-value">{row.installed.replace(/^Direct Drive /, '')}<span className="dropdown-sub">{row.nozzle} mm</span></span></span>;
    return <div key={i} className={grid ? 'opt-line extruder-row' : 'extruder-row'}>
      <span className={grid ? 'opt-label' : 'extruder-name'}>{label}</span>{grid ? <span className="opt-field">{control}</span> : control}
    </div>;
  })}</div>;
}
