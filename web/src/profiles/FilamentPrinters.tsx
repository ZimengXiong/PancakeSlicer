import { sameValue } from '../state/value-equality';
import { useMemo, useState } from 'react';
import type { Config, ConfigValue } from '../api/types';
import { getState, setState, toast, useStore, type State } from '../state/store';
import { useMachine } from '../flow/fsm';
import { lineRead } from '../catalog/read';
import { filamentScope, type PrinterSide } from '../flow/filament-scope';
import { changedKeys} from './model';
import { calibrationAt, calibrationTarget } from './context';
import { nozzleCount, recordOf, useAskedValues, valueBelow } from './view';
import type { LayerValues } from '../flow/resolved';
import { OPTIONS, useTab } from '../options';
import type { FilamentRecord } from './records';
import { commitRecords } from './transactions';
import { extruderLabel } from '../multinozzle/config';
import type { CalibrationRow } from './ownership';
import type { ProfileDraft } from './ProfileEditor';
import { isCalibrationKey } from './save-to-profile';
import { randomId } from '../random-id';
import { tabKeys } from './PrintOverrides';

interface Side extends PrinterSide { key: string; nozzle: number }

export const productIdOf = (s: State, filament: string) => (recordOf(s, 'filament', filament) as FilamentRecord | undefined)?.product ?? '';

function librarySides(s: State): Side[] {
  return s.records.printers.filter((p) => !p.archived).flatMap((p) => {
    const printer = p.id, count = nozzleCount(s, printer), bambu = p.vendor === 'BBL';
    return Array.from({ length: count }, (_, extruder) => {
      const nozzle = calibrationTarget(s, printer, extruder).nozzle;
      return { printer, extruder, nozzle, key: `${printer}#${extruder}`, label: count > 1 ? `${p.name}, ${extruderLabel(extruder, count, bambu)} ${nozzle} mm` : p.name };
    });
  });
}

const filamentValue = (k: string, l: LayerValues | undefined) => l?.record ?? valueBelow(l, 'tuning') ?? OPTIONS[k]?.default as ConfigValue | undefined;
function overrideKeys(layers: Record<string, LayerValues>, values: Config): string[] {
  return tabKeys('filament').filter((k) => values[k] !== undefined && !sameValue(k, values[k], filamentValue(k, layers[k])));
}

function SideRow({ filament, side, disabled, dirty, ready }: { filament: string; side: Side; disabled: boolean; dirty: boolean; ready: boolean }) {
  const asked = useAskedValues({ type: 'filament', id: filament, printer: side.printer, extruder: side.extruder }), { desc } = useTab('filament');
  const keys = useMemo(() => asked && desc ? overrideKeys(asked.layers, asked.values) : [], [asked, desc]);
  const mine = keys.filter((k) => asked?.layers[k]?.calibration !== undefined).length;
  const summary = !ready || asked?.state !== 'ready' || !desc ? 'Loading…' : keys.length ? countText(keys.length, 'setting', 'settings') : 'Uses the filament\'s settings';
  return <li><button type="button" className="fp-row" disabled={disabled}
    onClick={() => filamentScope(filament).send({ type: 'OPEN', side: { printer: side.printer, extruder: side.extruder, label: side.label }, dirty })}>
    <span className="fp-name">{side.label}</span><span className="fp-summary">{summary}</span>
    {mine > 0 && <span className="fp-changed">{countText(mine, 'changed by you', 'changed by you')}</span>}
  </button></li>;
}

const countText = (n: number, one: string, many: string) => `${n} ${n === 1 ? one : many}`;

export function FilamentPrinters({ filament, dirty }: { filament: string; dirty: boolean }) {
  useStore((s) => s.records);
  const s = getState(), product = lineRead('filaments', productIdOf(s, filament));
  const read = useMachine(product);
  const sides = useMemo(() => librarySides(s), [s.records]); // eslint-disable-line react-hooks/exhaustive-deps
  if (!sides.length) return <section className="filament-printers"><p className="muted">Add a printer to your library so you can tune this filament for it.</p></section>;
  return <section className="filament-printers" aria-label="Printers">
    {read.value === 'failed' && <div className="pf-editor-error" role="alert">{read.context.error}<button type="button" className="btn" onClick={() => product.send({ type: 'RETRY' })}>Retry</button></div>}
    {dirty && <p className="muted">Save or revert your changes to this filament before opening a printer's values.</p>}
    <ul className="fp-rows">{sides.map((side) => <SideRow key={side.key} filament={filament} side={side} dirty={dirty} disabled={dirty || read.value !== 'ready'} ready={read.value === 'ready'}/>)}</ul>
  </section>;
}

const draftKey = (filament: string, side: PrinterSide) => JSON.stringify(['filament-printer', filament, side.printer, side.extruder]);

export interface PrinterTuning { rows: Config; defaults: Config; fixed(key: string): string | undefined; setRows(next: Config): void; dirty: boolean; save(): void; revert(): void; ready: boolean }
export function usePrinterTuning(filament: string, side: PrinterSide): PrinterTuning {
  const key = draftKey(filament, side), draft = useStore((s) => s.libraryDrafts[key]);
  const asked = useAskedValues({ type: 'filament', id: filament, printer: side.printer, extruder: side.extruder });
  const saved = asked?.values ?? EMPTY, layers = asked?.layers ?? NO_LAYERS;
  const values = draft?.values ?? saved, cleared = draft?.cleared ?? [];
  const changes = useMemo(() => changedKeys(values, saved), [values, saved]);
  const [adding, setAdding] = useState<string[]>([]);
  useTab('filament');
  const write = (next: Config, clear = cleared) => setState((st) => {
    const drafts = { ...st.libraryDrafts };
    if (Object.keys(changedKeys(next, saved)).length || clear.length) drafts[key] = { values: next, baseline: saved, cleared: clear, origin: undefined, printer: side.printer } as ProfileDraft; else delete drafts[key];
    return { libraryDrafts: drafts };
  });
  const rows = useMemo(() => Object.fromEntries([...new Set([...overrideKeys(layers, values), ...adding])].filter((k) => values[k] !== undefined).map((k) => [k, values[k]!])) as Config, [layers, values, adding]);
  const defaults = useMemo(() => Object.fromEntries(tabKeys('filament').flatMap((k) => { const v = filamentValue(k, layers[k]); return v === undefined ? [] : [[k, v]]; })) as Config, [layers]);
  const setRows = (next: Config) => {
    const vals = { ...values }, clear = new Set(cleared), add = new Set(adding);
    const put = (k: string, v: Config[string]) => { const below = valueBelow(layers[k], 'calibration');
      if (below !== undefined && sameValue(k, v, below)) { if (layers[k]?.calibration !== undefined) { vals[k] = below; clear.add(k); } else if (saved[k] === undefined) delete vals[k]; else vals[k] = saved[k]; }
      else { vals[k] = v; clear.delete(k); } };
    for (const k of new Set([...Object.keys(rows), ...Object.keys(next)])) {
      if (!(k in next)) { add.delete(k); const base = filamentValue(k, layers[k]); if (base !== undefined) put(k, base); }
      else if (!(k in rows)) add.add(k);
      else if (!sameValue(k, next[k], values[k])) put(k, next[k]!);
    }
    setAdding([...add].filter((k) => k in next));
    write(vals, [...clear]);
  };
  const scope = filamentScope(filament);
  const save = () => {
    if (!scope.can({ type: 'SAVE' })) return;
    const s = getState(), current = calibrationAt(s, filament, side.printer, side.extruder), owned: Config = { ...current?.values };
    for (const k of Object.keys(changes)) owned[k] = values[k]!;
    for (const k of cleared) delete owned[k];
    if (Object.keys(changes).some((k) => k.startsWith('nozzle_temperature'))) delete owned.temperature_offset;
    const target = recordOf(s, 'filament', filament);
    if (!target) return;
    const next: CalibrationRow = current ? { ...current, revision: current.revision + 1, values: owned }
      : { id: `calibration:${randomId()}`, revision: 0, filament: target.id, printer: side.printer, ...calibrationTarget(s, side.printer, side.extruder), values: owned };
    scope.send({ type: 'SAVE' });
    const request = scope.get().context.request;
    const empty = !Object.keys(owned).length;
    if (empty && !current) { revert(); return; }
    commitRecords([{ kind: 'calibration', record: next, remove: empty }], { label: `Save for ${side.label}` })
      .then(() => { setAdding([]); setState((st) => { const drafts = { ...st.libraryDrafts }; delete drafts[key]; return { libraryDrafts: drafts }; }); scope.send({ type: 'SAVED', request }); toast('saved', { label: side.label }); },
        (e) => scope.send({ type: 'FAIL', request, error: e instanceof Error ? e.message : String(e) }));
  };
  const revert = () => { setAdding([]); setState((st) => { const drafts = { ...st.libraryDrafts }; delete drafts[key]; return { libraryDrafts: drafts }; }); };
  return { rows, defaults, fixed: (k) => isCalibrationKey(k) ? undefined : 'Set by this printer\'s tuning', setRows, dirty: Object.keys(changes).length > 0 || cleared.length > 0, save, revert, ready: asked?.state === 'ready' };
}
const EMPTY: Config = {}, NO_LAYERS: Record<string, LayerValues> = {};
