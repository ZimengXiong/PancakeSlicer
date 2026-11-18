import { Dropdown } from './Dropdown';
import { Fragment, useEffect, useState } from 'react';
import { effectiveConfig, getState, markStale, presetEdits, setOption, setState, toast, useStore } from '../state/store';
import type { ConfigValue } from '../api/types';
import { undoable } from '../state/history';
import { headLabel, loadHead, loadHeadIndex, loadHeadJson, saveUserHead, slopeMap, type Head, type HeadBox } from '../heads';
import { InfoTip } from './InfoTip';
import { Modal, ModalBody, ModalFooter, ModalTitle } from './Modal';
import { runCalibration } from './CalibDialog';

type ClearanceStep = { slope: number; relief: number };
const STEPS: ClearanceStep[] = [[12, 0.8], [15, 1.2], [18, 1.8], [21, 2.6], [24, 3.6], [27, 4.6], [30, 5.8], [32, 7.2]].map(([slope, relief]) => ({ slope, relief }));
const TILT_SLOPES = [0, 10, 20, 30, 40];
const TILT_MULTIPLIERS = [0.9, 0.95, 1, 1.05, 1.1];
const TOP_SPEEDS = [60, 80, 100];
const TOP_FANS = [0, 50, 100];
const CONE_APEX_RADIUS = 1.5;

const rad = (deg: number) => (deg * Math.PI) / 180;
const footRadius = (s: ClearanceStep) => s.relief / Math.tan(rad(s.slope)) + 0.5 * CONE_APEX_RADIUS;
function coneHeight(s: ClearanceStep, r: number) {
  const t = Math.tan(rad(s.slope)), foot = footRadius(s);
  if (r >= CONE_APEX_RADIUS) return Math.max(0, (foot - r) * t);
  return (foot - CONE_APEX_RADIUS) * t + (t * (CONE_APEX_RADIUS * CONE_APEX_RADIUS - r * r)) / (2 * CONE_APEX_RADIUS);
}

function stepClearance(head: Head, s: ClearanceStep, gap: number) {
  const foot = footRadius(s);
  let worst = Infinity;
  for (let ring = 0; ring <= 8; ring++) {
    const rho = (foot * ring) / 8, tip = coneHeight(s, rho);
    for (let k = 0; k < 72; k++) {
      const phi = (2 * Math.PI * k) / 72, ax = rho * Math.cos(phi), ay = rho * Math.sin(phi);
      for (const box of head.boxes) {
        const grow = box.nozzle ? 0 : head.lateral_margin;
        const nx = box.infinite_x ? ax : Math.min(Math.max(ax, box.min[0] - grow), box.max[0] + grow);
        const ny = box.infinite_y ? ay : Math.min(Math.max(ay, box.min[1] - grow), box.max[1] + grow);
        let z = box.min[2];
        if (box.nozzle) {
          const hx = Math.max(Math.abs(box.min[0]), Math.abs(box.max[0])), hy = Math.max(Math.abs(box.min[1]), Math.abs(box.max[1]));
          if (Math.hypot(hx, hy) <= head.tip_allowance.radius) z = Math.max(z, head.tip_allowance.height);
          if (z >= box.max[2]) continue;
        } else z -= head.margin;
        worst = Math.min(worst, gap + z - (coneHeight(s, Math.hypot(nx - ax, ny - ay)) - tip));
      }
    }
  }
  return worst;
}

const FIT_MARGIN = 0.2, TOUCH_MARGIN = 0.05;
const finiteBody = (b: HeadBox) => !b.nozzle && !b.infinite_x && !b.infinite_y;

function fitClearance(base: Head, json: Record<string, unknown>, lastClean: number, gap: number, id: string, name: string) {
  const shroud: Head = { ...base, boxes: base.boxes.filter(finiteBody) };
  if (!shroud.boxes.length) throw new Error('This print head model only defines the nozzle tip, so there\'s no upper body to calibrate.');
  const lowest = Math.min(...shroud.boxes.map((b) => b.min[2]));
  const needed = (k: number) => -stepClearance(shroud, STEPS[k - 1], gap);
  let shift = lastClean === 0 ? needed(1) - Math.max(gap, 0.1) : Math.max(...STEPS.slice(0, lastClean).map((_, k) => needed(k + 1)));
  shift += FIT_MARGIN;
  if (lastClean < STEPS.length) shift = Math.min(shift, needed(lastClean + 1) - TOUCH_MARGIN);
  shift = Math.max(shift, -lowest);
  const floor = lastClean > 0 ? STEPS[lastClean - 1].relief + base.margin - gap : -Infinity;
  const boxes = (json.boxes as HeadBox[]).map((b) => {
    const min = [...b.min] as HeadBox['min'], max = [...b.max] as HeadBox['max'];
    if (finiteBody(b)) { min[2] += shift; max[2] += shift; }
    else if ((b.infinite_x || b.infinite_y) && min[2] < floor) { max[2] += floor - min[2]; min[2] = floor; }
    return { ...b, min, max };
  });
  const head: Head = { ...base, id, name, calibrated: true, boxes };
  const step = lastClean === 0 ? 'none' : `${lastClean} (${STEPS[lastClean - 1].slope} deg, ${STEPS[lastClean - 1].relief} mm)`;
  const provenance = `${base.provenance ? `${base.provenance} ` : ''}Calibrated from ${base.id} with the non-planar clearance plate at layer height ${gap.toFixed(2)} mm: last clean step ${step}; body moved ${shift.toFixed(2)} mm.`;
  const steep = STEPS[STEPS.length - 1].slope;
  let lo = 0, hi = 100;
  if (stepClearance(head, { slope: steep, relief: hi }, gap) >= 0) lo = hi;
  for (let i = 0; i < 40 && hi - lo > 1e-3; i++) {
    const mid = 0.5 * (lo + hi);
    if (stepClearance(head, { slope: steep, relief: mid }, gap) >= 0) lo = mid; else hi = mid;
  }
  return {
    json: { ...json, id, name, calibrated: true, provenance, boxes }, shift, band: lo,
    slopeCap: (Math.min(...slopeMap(head, 72, gap)) * 180) / Math.PI,
    gantryFloor: Math.min(...boxes.filter((b) => b.infinite_x || b.infinite_y).map((b) => b.min[2])),
  };
}

const stepText = (k: number) => (k === 0 ? 'None (the print head already touched Cone 1)' : `Cone ${k}: ${STEPS[k - 1].slope}°, ${STEPS[k - 1].relief} mm`);
const firstTouchText = (k: number) => (k > STEPS.length ? 'no cone' : `cone ${k}`);
const logValue = (log: string[] | undefined, key: string) => log?.find((l) => l.startsWith(`${key} `))?.slice(key.length + 1);
const list = (v: ConfigValue | undefined) => (Array.isArray(v) ? v : String(v ?? '').split(/[,;]/)).map((x) => Number(x));
const fmt = (x: number) => String(Math.round(x * 1000) / 1000);

function writeProcess(name: string, keys: Record<string, ConfigValue>) {
  undoable(name, () => setState((s) => ({ dirty: { ...s.dirty, process: { ...presetEdits(s, 'process'), ...keys } }, job: markStale(s.job) })));
}

const memo = { page: 0, lastClean: Math.floor(STEPS.length / 2), tilt: TILT_SLOPES.map(() => Math.floor(TILT_MULTIPLIERS.length / 2)), speed: TOP_SPEEDS.length - 1, fan: 0, verifyHead: '', tiltCurve: null as number[] | null };

function Choice({ label, items, value, onChange }: { label: string; items: string[]; value: number; onChange(i: number): void }) {
  return (
    <label className="form-row">
      <span>{label}</span>
      <Dropdown className="combo" value={value} onValueChange={(next) => onChange(Number(next))} options={[...items.map((it, i) => ({ value: i, label: it }))]} />
    </label>
  );
}

const PAGES = ['Clearance', 'Tilt flow', 'Curved top', 'Check head'];

export default function NonplanarCalibDialog() {
  const s = useStore((st) => st);
  const machine = effectiveConfig(s, 'machine'), process = effectiveConfig(s, 'process');
  const headId = String(machine.nonplanar_head ?? '');
  const layerHeight = Number(process.layer_height) || 0.2;
  const [page, setPage] = useState(memo.page);
  const [lastClean, setLastClean] = useState(memo.lastClean);
  const [tilt, setTilt] = useState(memo.tilt);
  const [speed, setSpeed] = useState(memo.speed);
  const [fan, setFan] = useState(memo.fan);
  const [verifyHead, setVerifyHead] = useState(memo.verifyHead || headId);
  const [head, setHead] = useState<Head | null>(null);
  const [newId, setNewId] = useState(`${headId || 'generic-conservative'}-calibrated`);
  useEffect(() => setNewId(`${headId || 'generic-conservative'}-calibrated`), [headId]);
  const [result, setResult] = useState<Record<number, { rows: [string, string][]; caption?: string }>>({});
  Object.assign(memo, { page, lastClean, tilt, speed, fan, verifyHead });
  useEffect(() => void loadHeadIndex().catch(() => undefined), []);
  useEffect(() => void loadHead(headId).then(setHead), [headId]);
  const close = () => setState({ calibDialog: null });
  const generate = (test: 'clearance' | 'tilt-flow' | 'curved-top' | 'verifier') => {
    const curve = list(process.nonplanar_tilt_flow);
    void runCalibration(`nonplanar-${test}`, '', test === 'verifier' ? verifyHead : undefined).then((started) => {
      if (started && test === 'tilt-flow') memo.tiltCurve = curve;
    });
  };
  const writeHead = async () => {
    try {
      if (headId === 'calibration-probe') throw new Error("Your printer is currently set to the calibration test probe—pick your printer's actual print head first.");
      const json = await loadHeadJson(headId);
      if (!head || !json) throw new Error('The generic fallback head can\'t be calibrated directly—select your printer\'s own print head model first.');
      const fit = fitClearance(head, json, lastClean, layerHeight, newId, `${head.name} (calibrated)`);
      await saveUserHead(fit.json);
      setOption('machine', 'nonplanar_head', newId);
      setVerifyHead(newId);
      setResult({ ...result, 0: { rows: [['Head', newId], ['Body moved', `${fit.shift.toFixed(2)} mm`], ['Steepest plane', `${fit.slopeCap.toFixed(1)}°`],
        [`Relief at ${STEPS[STEPS.length - 1].slope}°`, `${fit.band.toFixed(2)} mm`], ['Gantry floor', `${fit.gantryFloor.toFixed(1)} mm`]] } });
    } catch (e) {
      toast('couldnt_save_calibrated_print', { detail: e instanceof Error ? e.message : String(e) });
    }
  };
  const writeTilt = () => {
    let base = memo.tiltCurve ?? list(process.nonplanar_tilt_flow);
    if (base.length !== TILT_SLOPES.length || base.some((x) => !Number.isFinite(x))) base = TILT_SLOPES.map(() => 1);
    const curve = tilt.map((p, i) => Math.round(Math.min(1.5, Math.max(0.5, base[i] * TILT_MULTIPLIERS[p])) * 1000) / 1000);
    const value = curve.map(fmt).join(',');
    writeProcess('Update "Curved top flow by slope"', { nonplanar_tilt_flow: value });
    setResult({ ...result, 1: { rows: [['Tilt flow', value]], caption: `Saved tilt flow ${value} to your print profile! Don't forget to save the profile to keep it.` } });
  };
  const writeTop = () => {
    writeProcess('Non-planar calibration - Curved-top quality', { nonplanar_top_speed_factor: `${TOP_SPEEDS[speed]}%`, nonplanar_top_fan_boost: `${TOP_FANS[fan]}%` });
    setResult({ ...result, 2: { rows: [['Curved top speed', `${TOP_SPEEDS[speed]}%`], ['Fan boost', `${TOP_FANS[fan]}%`]] } });
  };
  const check = () => {
    const job = getState().job, log = job.calibLog;
    const verified = Number(logValue(log, 'calib_verifier_first_touch')), model = Number(logValue(log, 'calib_model_first_touch'));
    if (job.calib !== 'nonplanar-verifier' || job.state !== 'done' || job.stale || !Number.isFinite(verified) || !Number.isFinite(model))
      return toast('click_generate_slice_clearance');
    const expected = verifyHead || 'generic-conservative';
    if (logValue(log, 'calib_head') !== expected) return toast('plate_was_sliced_generate', { value: logValue(log, 'calib_head'), expected });
    const measured = lastClean + 1;
    setResult({ ...result, 3: { rows: [['First touch, clearance check', firstTouchText(verified)], ['First touch, head model', firstTouchText(model)],
      ['First touch, your print', firstTouchText(measured)], ['Clearance check', verified === measured ? 'Agrees' : 'Disagrees']],
      caption: `First contact — Clearance check: ${firstTouchText(verified)} | Head model: ${firstTouchText(model)} | Your print: ${firstTouchText(measured)}.\n` + (verified === measured ? "Nice! The clearance check matches your test print." : "The clearance check doesn't quite match your print—head over to the Head clearance tab to calibrate your print head.") } });
  };
  const sliced = s.job.calib === 'nonplanar-clearance' && logValue(s.job.calibLog, 'calib_head') === (headId || 'generic-conservative') ? s.job.calibLog!.filter((l) => l.startsWith('calib_step ')) : [];
  const verdict = (k: number) => {
    const c = head ? stepClearance(head, STEPS[k], layerHeight) : Number(sliced[k]?.split(' ')[4]);
    return Number.isFinite(c) ? ` — ${c >= 0 ? 'head model: clean' : 'head model: touches'}` : '';
  };
  const steps = [`Head ${headId || 'generic-conservative'}, layer height ${layerHeight} mm:`, ...STEPS.map((_, k) => stepText(k + 1) + verdict(k))].join('\n');
  const heads = (s.heads ?? []).map((h) => h.id).filter((id) => id !== 'calibration-probe');
  const intro = [
    'This test prints eight cones—each steeper and taller than the last—with clearance safety turned off so you can see where your nozzle sock, fan duct, or gantry starts touching the print. Pick the last cone that printed cleanly, then click Write head to save a custom print head profile for your machine.',
    'This test prints five rows of curved swatches at 0°, 10°, 20°, 30°, and 40° slopes, stepping flow from 0.90× to 1.10× across each row. Pick the smoothest swatch (no gaps or ridges) for each angle, then click Write tilt flow to save them to your profile.',
    'Front row: curved top speed 60–100%; right column: fan boost 0–100%; pick the cleanest of each.',
    'Checks your selected print head against the sliced test plate. The first cone it flags should match the cone right after the last clean one on your actual print. Click Generate, slice the plate, and then run the check.',
  ][page];
  return (
    <Modal size="l" label="Non-planar calibration" onClose={close}>
      <ModalTitle title="Non-planar calibration" onClose={close} />
      <ModalBody className="modal-fields">
        <span className="seg np-calib-tabs" role="tablist" aria-label="Calibration">
          {PAGES.map((p, i) => <button key={p} type="button" role="tab" aria-selected={i === page} aria-checked={i === page} onClick={() => setPage(i)}>{p}</button>)}
        </span>
        <div className="form-row"><span>Test plate<InfoTip label={PAGES[page]}>{intro}</InfoTip></span>
          <span><button type="button" className="btn" onClick={() => generate(page === 0 ? 'clearance' : page === 1 ? 'tilt-flow' : page === 2 ? 'curved-top' : 'verifier')}>Generate</button></span></div>
        {page === 0 && <>
          <p className="flush-note ask-message">{steps}</p>
          <Choice label="Last clean cone" items={STEPS.map((_, k) => stepText(k)).concat(stepText(STEPS.length))} value={lastClean} onChange={setLastClean} />
          <label className="form-row"><span>New head ID</span><input className="text" value={newId} onChange={(e) => setNewId(e.target.value)} /></label>
        </>}
        {page === 1 && TILT_SLOPES.map((slope, i) => (
          <Choice key={slope} label={`Best patch at ${slope}°`} items={TILT_MULTIPLIERS.map((m, j) => `Patch ${j + 1} (×${m.toFixed(2)})`)} value={tilt[i]}
            onChange={(v) => setTilt(tilt.map((x, j) => (j === i ? v : x)))} />
        ))}
        {page === 2 && <>
          <Choice label="Best speed (front row)" items={TOP_SPEEDS.map((x) => `${x}% of top surface speed`)} value={speed} onChange={setSpeed} />
          <Choice label="Best fan (right column)" items={TOP_FANS.map((x) => `${x}% fan boost`)} value={fan} onChange={setFan} />
        </>}
        {page === 3 && <label className="form-row"><span>Head</span>
          <Dropdown className="combo" value={verifyHead} onValueChange={setVerifyHead} options={[...(heads.includes(verifyHead) ? heads : [verifyHead, ...heads]).map((id) => ({ value: id, label: id ? headLabel(id) : headLabel('') }))]} />
        </label>}
        {result[page] && <div role="status"><dl className="modal-grid">{result[page].rows.map(([k, v]) => <Fragment key={k}><dt>{k}</dt><dd>{v}</dd></Fragment>)}</dl>
          {result[page].caption && <p className="ask-message">{result[page].caption}</p>}</div>}
      </ModalBody>
      <ModalFooter>
        <button type="button" className="btn" onClick={close}>Close</button>
        <button type="button" className="btn-primary" onClick={() => (page === 0 ? void writeHead() : page === 1 ? writeTilt() : page === 2 ? writeTop() : check())}>
          {['Write head', 'Write tilt flow', 'Write curved-top settings', 'Check sliced plate'][page]}</button>
      </ModalFooter>
    </Modal>
  );
}
