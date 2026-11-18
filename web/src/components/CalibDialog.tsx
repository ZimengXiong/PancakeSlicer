import { Check } from './Check';
import { useState } from 'react';
import { newProject, slice } from '../actions';
import { effectiveConfig, getState, setState, toast, type CalibMenu } from '../state/store';
import { Dropdown } from './Dropdown';
import { Modal, ModalBody, ModalFooter, ModalTitle } from './Modal';

type Values = Record<string, string>;
type Field =
  | { key: string; label: string; unit?: string; list?: boolean; show?: (v: Values) => boolean }
  | { key: string; label: string; check: true; show?: (v: Values) => boolean };
type Group = { legend: string; key: string; options: [string, string][]; preset?: (choice: string, v: Values) => Values };
interface Calib {
  title: string;
  groups?: Group[];
  settings: string;
  fields: Field[];
  note?: string;
  wiki: string;
  defaults(cfg: ReturnType<typeof printer>): Values;
  run(v: Values, cfg: ReturnType<typeof printer>): string | { kind: string; params: Values };
}

const num = (v: string) => Number(v);
const first = (v: unknown) => String(Array.isArray(v) ? v[0] ?? '' : v ?? '');

function printer() {
  const s = getState();
  const m = effectiveConfig(s, 'machine'), f = effectiveConfig(s, 'filament', 0);
  const flavor = first(m.gcode_flavor);
  return {
    flavor,
    reprap: flavor === 'reprapfirmware',
    junction: flavor === 'marlin2' && Number(first(m.machine_max_junction_deviation)) > 0,
    bowden: first(m.extruder_type) === 'Bowden',
    filament: first(f.filament_type).toUpperCase(),
  };
}

const TEMPS: Record<string, [number, number]> = { PLA: [230, 190], 'ABS/ASA': [270, 230], PETG: [250, 230], PCTG: [280, 240], TPU: [240, 210], 'PA-CF': [320, 280], 'PET-CF': [320, 280] };
const tempType = (filament: string) => filament === 'ABS' || filament === 'ASA' ? 'ABS/ASA' : TEMPS[filament] ? filament : 'PLA';

const shapers = (flavor: string): [string, string][] =>
  (flavor === 'klipper' ? ['Default', 'ZV', 'MZV', 'ZVD', 'EI', '2HUMP_EI', '3HUMP_EI']
    : flavor === 'reprapfirmware' ? ['Default', 'MZV', 'ZVD', 'ZVDD', 'ZVDDD', 'EI2', 'EI3', 'DAA']
    : flavor === 'marlin2' ? ['ZV'] : ['Default']).map((t) => [t, t]);
const firmwareNote = (flavor: string) => flavor === 'marlin2' || flavor === 'marlin' ? 'Marlin version => 2.1.2\nFixed-Time motion not yet implemented.'
  : flavor === 'klipper' ? 'Klipper version => 0.9.0'
  : flavor === 'reprapfirmware' ? 'RepRap firmware version => 3.4.0\nCheck your firmware documentation for supported shaper types.'
  : 'Heads up: make sure your printer\'s firmware supports the calibration option you choose here.';

const MODELS: [string, string][] = [['0', 'Ringing Tower'], ['1', 'Fast Tower']];
const range = (start: string, end: string, step: string, v: Values) => num(v[start]) > 0 && num(v[step]) >= 0 && num(v[end]) > num(v[start]) + num(v[step]);

const paDefaults = (method: string, bowden: boolean): Values => ({
  start: '0', end: bowden ? '1' : method === 'pa-pattern' ? '0.08' : '0.1',
  step: method === 'pa-pattern' ? (bowden ? '0.05' : '0.005') : bowden ? '0.02' : '0.002',
  print_numbers: method === 'pa-tower' ? '0' : '1',
});

export const CALIBS: Record<Exclude<CalibMenu, 'nonplanar'>, Calib> = {
  temperature: {
    title: 'Temperature calibration', settings: 'Settings', wiki: 'temp_calib',
    groups: [{ legend: 'Filament type', key: 'type', options: [...Object.keys(TEMPS), 'Custom'].map((t) => [t, t]),
      preset: (t): Values => (TEMPS[t] ? { start: String(TEMPS[t][0]), end: String(TEMPS[t][1]) } : {}) }],
    fields: [{ key: 'start', label: 'Start temp: ', unit: '℃' }, { key: 'end', label: 'End temp: ', unit: '℃' }, { key: 'resize', label: 'Auto-scale for nozzle', check: true }],
    defaults: (p) => { const t = tempType(p.filament); return { type: t, start: String(TEMPS[t][0]), end: String(TEMPS[t][1]), resize: '1' }; },
    run: (v) => num(v.start) > 500 || num(v.end) < 155 || num(v.start) < num(v.end) + 5
      ? 'Please input valid values:\nStart temp: <= 500\nEnd temp: >= 155\nStart temp >= End temp + 5'
      : { kind: 'temp-tower', params: { start: v.start, end: v.end, resize: v.resize } },
  },
  'max-flowrate': {
    title: 'Max volumetric speed test', settings: 'Settings', wiki: 'volumetric_speed_calib',
    fields: [{ key: 'start', label: 'Start volumetric speed: ', unit: 'mm³/s' }, { key: 'end', label: 'End volumetric speed: ', unit: 'mm³/s' }, { key: 'step', label: 'Step: ', unit: 'mm³/s' }],
    defaults: () => ({ start: '5', end: '20', step: '0.5' }),
    run: (v) => range('start', 'end', 'step', v) ? { kind: 'max-volumetric-speed', params: v } : 'Please input valid values:\nstart > 0\nstep >= 0\nend > start + step',
  },
  'pressure-advance': {
    title: 'PA Calibration', settings: 'Settings', wiki: 'pressure_advance_calib',
    groups: [
      { legend: 'Extruder type', key: 'extruder', options: [['dde', 'DDE'], ['bowden', 'Bowden']], preset: (e, v) => paDefaults(v.method, e === 'bowden') },
      { legend: 'Method', key: 'method', options: [['pa-tower', 'PA Tower'], ['pa-line', 'PA Line'], ['pa-pattern', 'PA Pattern']], preset: (m, v) => paDefaults(m, v.extruder === 'bowden') },
    ],
    fields: [
      { key: 'start', label: 'Start PA: ' }, { key: 'end', label: 'End PA: ' }, { key: 'step', label: 'PA step: ' },
      { key: 'accelerations', label: 'Accelerations: ', list: true, show: (v) => v.method === 'pa-pattern' },
      { key: 'speeds', label: 'Speeds: ', list: true, show: (v) => v.method === 'pa-pattern' },
      { key: 'print_numbers', label: 'Print numbers', check: true, show: (v) => v.method === 'pa-line' },
    ],
    defaults: (p) => ({ extruder: p.bowden ? 'bowden' : 'dde', method: 'pa-tower', accelerations: '', speeds: '', ...paDefaults('pa-tower', p.bowden) }),
    run: (v) => {
      if (num(v.start) < 0 || num(v.step) < 0.001 || num(v.end) <= num(v.start) + num(v.step))
        return 'Please input valid values:\nStart PA: >= 0.0\nEnd PA: > Start PA\nPA step: >= 0.001';
      const list = (s: string) => s.split(/[,\s]+/).filter(Boolean).map(Number).filter((x) => x > 0).join(';');
      const params: Values = { start: v.start, end: v.end, step: v.step };
      if (v.method === 'pa-line') params.print_numbers = v.print_numbers;
      if (v.method === 'pa-pattern') Object.assign(params, { accelerations: list(v.accelerations), speeds: list(v.speeds) });
      return { kind: v.method, params };
    },
  },
  'flow-ratio': {
    title: 'Flow Ratio Calibration', settings: 'Settings', wiki: 'flow_ratio_calib',
    groups: [{ legend: 'Calibration Test Type', key: 'test', options: [['classic-1', 'Pass 1 (Coarse)'], ['classic-2', 'Pass 2 (Fine)'], ['yolo-1', 'YOLO (Recommended)'], ['yolo-2', 'YOLO (Perfectionist)']] },
      { legend: 'Top Surface Pattern', key: 'pattern', options: [['archimedean', 'Archimedean Chords'], ['monotonic', 'Monotonic']] }],
    fields: [],
    defaults: () => ({ test: 'yolo-1', pattern: 'archimedean' }),
    run: (v) => { const [type, pass] = v.test.split('-'); return { kind: 'flow-rate', params: { type, pass, pattern: v.pattern } }; },
  },
  retraction: {
    title: 'Retraction', settings: 'Settings', wiki: 'retraction_calib',
    fields: [{ key: 'start', label: 'Start retraction length: ', unit: 'mm' }, { key: 'end', label: 'End retraction length: ', unit: 'mm' }, { key: 'step', label: 'Step: ', unit: 'mm' }],
    defaults: () => ({ start: '0', end: '2', step: '0.1' }),
    run: (v) => num(v.start) >= 0 && num(v.step) >= 0 && num(v.end) > num(v.start) + num(v.step) ? { kind: 'retraction', params: v } : 'Please input valid values:\nstart > 0\nstep >= 0\nend > start + step',
  },
  cornering: {
    title: 'Cornering test', settings: 'Cornering settings', wiki: 'cornering_calib',
    groups: [{ legend: 'Test model', key: 'model', options: [...MODELS, ['2', 'SCV-V2']] }],
    fields: [{ key: 'start', label: 'Start: ' }, { key: 'end', label: 'End: ' }],
    note: 'Tip: Lower values give you sharper corners, though prints will run a bit slower.',
    defaults: (p) => ({ model: '0', ...(p.junction ? { start: '0.000', end: '0.250' } : { start: '1.000', end: '15.000' }) }),
    run: (v, p) => {
      const max = p.junction ? 0.3 : 100;
      return num(v.start) < 0 || num(v.end) > max || num(v.end) <= num(v.start)
        ? `Please input valid values:\n(0 <= Cornering <= ${max.toFixed(3)})` : { kind: 'cornering', params: v };
    },
  },
  'input-shaping-freq': {
    title: 'Input shaping Frequency test', settings: 'Frequency settings', wiki: 'input_shaping_calib',
    groups: [{ legend: 'Test model', key: 'model', options: MODELS }],
    fields: [
      { key: 'freq_start_x', label: 'X Start: ', unit: 'Hz' }, { key: 'freq_end_x', label: 'X End: ', unit: 'Hz' },
      { key: 'freq_start_y', label: 'Y Start: ', unit: 'Hz', show: () => !printer().reprap }, { key: 'freq_end_y', label: 'Y End: ', unit: 'Hz', show: () => !printer().reprap },
      { key: 'damping', label: 'Damp: ' },
    ],
    note: 'Tip: Leave Damp at 0 to use your printer\'s default or saved damping value.',
    defaults: (p) => ({ model: '0', shaper: shapers(p.flavor)[0][0], freq_start_x: '15', freq_end_x: '110', freq_start_y: '15', freq_end_y: '110', damping: '0.150' }),
    run: (v, p) => {
      const axes = p.reprap ? ['x'] : ['x', 'y'];
      if (axes.some((a) => !(num(v[`freq_start_${a}`]) > 0 && num(v[`freq_start_${a}`]) < num(v[`freq_end_${a}`]) && num(v[`freq_end_${a}`]) < 500)))
        return 'Please input valid values:\n(0 < FreqStart < FreqEnd < 500)';
      if (!(num(v.damping) >= 0 && num(v.damping) <= 1)) return 'Enter a damping factor between 0 and 1 (0 < zeta <= 1).';
      const { freq_start_y, freq_end_y, ...rest } = v;
      return { kind: 'input-shaping-freq', params: p.reprap ? rest : { ...rest, freq_start_y, freq_end_y } };
    },
  },
  'input-shaping-damp': {
    title: 'Input shaping Damp test', settings: 'Frequency settings', wiki: 'input_shaping_calib',
    groups: [{ legend: 'Test model', key: 'model', options: MODELS }],
    fields: [
      { key: 'freq_x', label: 'Frequency X: ', unit: 'Hz' }, { key: 'freq_y', label: 'Frequency Y: ', unit: 'Hz', show: () => !printer().reprap },
      { key: 'damping_start', label: 'Damp Start: ' }, { key: 'damping_end', label: 'Damp End: ' },
    ],
    note: 'Tip: Enter the X and Y frequencies you found in the frequency test.',
    defaults: (p) => ({ model: '0', shaper: shapers(p.flavor)[0][0], freq_x: '30', freq_y: '30', damping_start: '0.000', damping_end: '0.400' }),
    run: (v, p) => {
      if ([v.freq_x, ...(p.reprap ? [] : [v.freq_y])].some((f) => !(num(f) > 0 && num(f) < 500))) return 'Please input valid values:\n(0 < Freq < 500)';
      if (!(num(v.damping_start) >= 0 && num(v.damping_start) < num(v.damping_end) && num(v.damping_end) <= 1))
        return 'Enter a valid damping range (0 <= Start < End <= 1).';
      const { freq_y, ...rest } = v;
      return { kind: 'input-shaping-damp', params: p.reprap ? rest : { ...rest, freq_y } };
    },
  },
  vfa: {
    title: 'VFA test', settings: 'Settings', wiki: 'vfa_calib',
    fields: [{ key: 'start', label: 'Start speed: ', unit: 'mm/s' }, { key: 'end', label: 'End speed: ', unit: 'mm/s' }, { key: 'step', label: 'Step: ', unit: 'mm/s' },
      { key: 'resize', label: 'Auto-scale for nozzle', check: true }],
    defaults: () => ({ start: '40', end: '200', step: '10', resize: '1' }),
    run: (v) => num(v.start) > 10 && num(v.step) >= 0 && num(v.end) > num(v.start) + num(v.step) ? { kind: 'vfa', params: v } : 'Please input valid values:\nstart > 10\nstep >= 0\nend > start + step',
  },
};

const withShapers = (c: Calib, flavor: string): Calib => c.title.startsWith('Input shaping')
  ? { ...c, groups: [...(c.groups ?? []), { legend: 'Input shaper type', key: 'shaper', options: shapers(flavor) }] } : c;

export async function runCalibration(kind: string, params: string, head?: string) {
  const s = getState();
  if (s.objects.length || s.projectPlates.some((pl) => pl.objects.length)) {
    await newProject();
    const after = getState();
    if (after.objects.length || after.projectPlates.some((pl) => pl.objects.length)) return false;
  }
  setState({ calibDialog: null });
  void slice({ kind, params, head });
  return true;
}

export default function CalibDialog({ which }: { which: Exclude<CalibMenu, 'nonplanar'> }) {
  const p = printer();
  const c = withShapers(CALIBS[which], p.flavor);
  const [v, setV] = useState<Values>(() => c.defaults(p));
  const close = () => setState({ calibDialog: null });
  const ok = async () => {
    const result = c.run(v, p);
    if (typeof result === 'string') return toast('calibration_values_invalid', { detail: result });
    await runCalibration(result.kind, Object.entries(result.params).map(([k, x]) => `${k}=${x}`).join(','));
  };
  return (
    <Modal size="m" label={c.title} onClose={close}>
      <ModalTitle title={c.title} onClose={close} />
      <form onSubmit={(e) => { e.preventDefault(); void ok(); }}>
        <ModalBody className="modal-fields">
          {c.groups?.map((g) => {
            const pick = (value: string) => setV({ ...v, [g.key]: value, ...g.preset?.(value, v) });
            return <div key={g.key} className="modal-fields">
              <div className="form-row"><span>{g.legend.replace(/:\s*$/, '')}</span>{<Dropdown className="combo" aria-label={g.legend} value={v[g.key]} onValueChange={pick} options={g.options.map(([value, label]) => ({ value, label }))} />}</div>
              {g.key === 'shaper' && <p className="flush-note">{firmwareNote(p.flavor)}</p>}
            </div>;
          })}
          {c.fields.filter((f) => !f.show || f.show(v)).map((f) => 'check' in f ? (
            <label key={f.key} className="modal-check"><Check checked={v[f.key] === '1'} onCheckedChange={(next) => setV({ ...v, [f.key]: next ? '1' : '0' })} />{f.label.replace(/:\s*$/, '')}</label>
          ) : (
            <label key={f.key} className="form-row"><span>{f.label.replace(/:\s*$/, '')}</span>
              <span className="modal-unit"><input className={f.list ? 'text' : 'text num'} type={f.list ? 'text' : 'number'} step="any" value={v[f.key]}
                placeholder={f.list ? 'Comma-separated' : undefined} onChange={(e) => setV({ ...v, [f.key]: e.target.value })} />{f.unit && <span>{f.unit}</span>}</span>
            </label>
          ))}
          {c.note && <p className="flush-note">{c.note}</p>}
        </ModalBody>
        <ModalFooter lead={<a className="link" href={`https://www.orcaslicer.com/wiki/${c.wiki}`} target="_blank" rel="noreferrer">Guide</a>}>
          <button type="button" className="btn" onClick={close}>Cancel</button>
          <button type="submit" className="btn-primary">Create test</button>
        </ModalFooter>
      </form>
    </Modal>
  );
}
