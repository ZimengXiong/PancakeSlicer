import { useEffect, useId, useMemo, useRef, useState, type ReactNode } from 'react';
import { SEP } from '../separator';
import { openLibrary } from '../navigation';
import { useStore, getState } from '../state/store';
import { profileInfo } from '../profiles/view';
import { printerCover } from '../profiles/metadata';
import { confirmDialog } from '../components/Ask';
import { Check } from '../components/Check';
import { NumberBox } from '../components/Field';
import { Range } from '../components/Range';
import { responseError } from '../api/access';
import { controlPrinter, presenceStatus, statusLine, type PrinterConnection, type PrinterStatus, type StatusTone, type Tray } from './connections';
import { printerCapabilities } from './capabilities';
import { DeviceCamera } from './DeviceCamera';
import { materialNote, reportText, slotOf, useFilamentType, usePrinterSlots, type SlotReport } from './slots';
import { FilamentColourMenu } from '../profiles/FilamentColourMenu';
import { trayCode } from '../multinozzle/ams';
import { Dropdown } from '../components/Dropdown';
import { Skeleton } from '../components/Skeleton';
import { outcomeLabel, outcomeTone } from './sendOutcome';
import type { Transfer } from '../flow/transfer';
import { useDeviceControl, type DeviceCommand } from './device-controls';
import './printers.css';
import './device.css';
import { pickSlotFilament, reportedUnits, setupOf } from './device-slots';
import { NozzlesCard, SlotsCard } from './DeviceSlots';
import { feedOf } from './feeds';
import { RecordProblem } from '../profiles/RecordProblem';

function Card({ title, action, className = '', children }: { title: string; action?: ReactNode; className?: string; children: ReactNode }) {
  return <section className={`device-card ${className}`}>{action ? <div className="device-card-head"><h2>{title}</h2>{action}</div> : <h2>{title}</h2>}{children}</section>;
}

function ControlRow({ label, children }: { label: string; children(labelId: string): ReactNode }) {
  const id = useId();
  return <div className="device-row" role="group" aria-labelledby={id}><span id={id} className="device-row-label">{label}</span>{children(id)}</div>;
}

const ControlError = ({ error }: { error: string | null }) => error ? <span className="control-error device-control-error" role="status">{error}</span> : null;

function TemperatureRow({ printer, command, label, actual, target, min = 0, max, editable, offline }: { printer: string; command: DeviceCommand; label: string; actual: number; target?: number; min?: number; max: number; editable: boolean; offline: boolean }) {
  const ctl = useDeviceControl(printer, command, target);
  return <ControlRow label={label}>{(id) => <><span className="device-row-value"><span className="device-row-actual">{Math.round(actual)} °C</span>
    <span className="device-row-to">Target</span>
    {editable ? <NumberBox labelledBy={id} label={`${label} target`} value={ctl.value ?? 0} min={min} max={max} unit="°C" readOnly={offline} onCommit={ctl.commit} />
      : <span>{target != null ? Math.round(target) : '–'} °C</span>}</span><ControlError error={ctl.error} /></>}</ControlRow>;
}

function FanRow({ printer, fan, label, reported, min, max, offline }: { printer: string; fan: string; label: string; reported: number; min: number; max: number; offline: boolean }) {
  const ctl = useDeviceControl(printer, { control: 'fan', fan }, reported), value = ctl.value ?? reported, apply = ctl.commit;
  const [shown, show] = useState(value), timer = useRef<ReturnType<typeof setTimeout>>(undefined);
  useEffect(() => { if (!timer.current) show(value); }, [value]);
  useEffect(() => () => clearTimeout(timer.current), []);
  const send = (raw: number) => { const v = Math.min(max, Math.max(min, Math.round(raw / 10) * 10)); show(v); clearTimeout(timer.current); timer.current = setTimeout(() => { timer.current = undefined; apply(v); }, 300); };
  return <ControlRow label={label}>{(id) => <><span className="device-row-value device-fan">
    <Range aria-label={`${label} speed`} value={shown} min={min} max={max} step={10} disabled={offline} onValueChange={show} onValueCommit={send} />
    <NumberBox labelledBy={id} label={label} value={shown} min={min} max={max} unit="%" readOnly={offline} onCommit={send} /></span><ControlError error={ctl.error} /></>}</ControlRow>;
}

const duration = (seconds: number) => { const n = Math.max(0, Math.round(seconds / 60)); return n >= 60 ? `${Math.floor(n / 60)} h ${n % 60} min` : `${n} min`; };
const preciseDuration = (seconds: number) => { const n = Math.max(0, Math.floor(seconds)); return `${n >= 3600 ? `${Math.floor(n / 3600)}h ` : ''}${Math.floor(n / 60) % 60}m ${n % 60}s`; };
const clock = (date: string | number) => new Date(date).toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' });
const when = (date: string) => {
  const d = new Date(date), days = Math.round((new Date().setHours(0, 0, 0, 0) - new Date(date).setHours(0, 0, 0, 0)) / 864e5);
  return `${days === 0 ? 'Today' : days === 1 ? 'Yesterday' : d.toLocaleDateString([], { month: 'short', day: 'numeric' })} ${clock(date)}`;
};
const jobName = (file: string) => file.replace(/\.gcode(\.3mf)?$/i, '').replace(/\.3mf$/i, '');
const SPEEDS = [{ id: '1', label: 'Silent' }, { id: '2', label: 'Standard' }, { id: '3', label: 'Sport' }, { id: '4', label: 'Ludicrous' }] as const;
type History = Transfer;

function Spool({ printer, tray, trays, report, chosen, choices, busy, choose, swatch }: { printer: string; tray: Tray; trays: readonly Tray[];
  report?: SlotReport; chosen: string; choices: { key: string; name: string; type: string }[]; busy: boolean; choose(filament: string): void;
   swatch?: { printer: string; slot: number } }) {
  const colour = /^#[0-9a-f]{6}$/i.test(tray.color) ? tray.color : undefined;
  const remaining = tray.type && tray.remaining != null && tray.remaining >= 0 ? Math.min(100, tray.remaining) : undefined;
  const where = trayCode(tray, trays);
  const mine = choices.find((c) => c.key === chosen), mineType = useFilamentType(printer, chosen, tray.extruder ?? 0), note = mine ? materialNote(tray.type, mine.name, mineType) : null, earlier = !chosen && report?.earlier ? choices.find((c) => c.key === report.earlier) : undefined;
  return <div className="device-spool" data-active={tray.active || undefined} aria-label={`${where}, ${tray.type || 'empty'}${tray.active ? ', in use' : ''}`}>
    {swatch ? <span className="device-spool-swatch"><FilamentColourMenu printer={swatch.printer} slot={swatch.slot}/></span>
      : <span className="device-spool-disc" style={{ backgroundColor: tray.type ? colour : undefined }} data-empty={!tray.type || undefined} aria-hidden="true" />}
    <span className="device-spool-text">
      <Dropdown className="combo device-spool-pick" value={chosen} placeholder="Pick a filament…" disabled={busy} aria-label={`Your filament in ${where}`}
        options={[{ value: '', label: 'Pick a filament…' }, ...choices.map((c) => ({ value: c.key, label: c.name, search: `${c.name} ${c.type}` }))]}
        onValueChange={(next) => next !== chosen && choose(next)} />
      <span className="device-spool-report">{reportText(report ?? { tray: { type: tray.type, colour: tray.color, name: '', filament_id: '' }, remaining: remaining ?? null })}{tray.active && <span className="status-text" data-tone="printing">, in use</span>}</span>
      {earlier && <button type="button" className="link device-spool-earlier" disabled={busy} onClick={() => choose(earlier.key)}>Your earlier choice: {earlier.name}</button>}
      {note && <span className="device-spool-why" role="note">{note}</span>}</span>
    {remaining != null && <progress aria-label={`${where} remaining`} max="100" value={remaining} />}
  </div>;
}

function jobEnd(st: PrinterStatus | undefined): { label: string; tone?: StatusTone; reason?: string } {
  if (!st?.online) return { label: statusLine(st) || 'Connecting…', tone: 'offline' };
  const reason = st.messages?.length ? undefined : st.detail?.match(/error ([0-9A-F]{8})/i)?.[1];
  switch (st.state) {
    case 'failed': return { label: 'Print failed', tone: 'error', reason: reason && `Error ${reason}` };
    case 'error': return { label: 'Printer error', tone: 'error', reason: reason && `Error ${reason}` };
    case 'finished': return { label: 'Print finished' };
    case 'busy': return { label: 'Printer busy' };
    default: return { label: st.detail === 'cancelled' ? 'Print cancelled' : 'Ready to print' };
  }
}

function PrinterRender({ cover }: { cover?: string }) {
  const [failed, fail] = useState<string>();
  return cover && failed !== cover ? <img src={cover} alt="" onError={() => fail(cover)} /> : null;
}

const HISTORY_PAGE = 20;
const jobReason = (job: History) => job.job_end === 'failed' ? (job.job_errors ?? []).map((e) => e.message ? `${e.message} (${e.code})` : `Error ${e.code}`).join(SEP) : '';

function HistoryRow({ job, running, printer }: { job: History; running: boolean; printer?: boolean }) {
  const reason = jobReason(job);
  return <li>
    <strong title={job.filename}>{jobName(job.filename)}</strong>{printer && <span className="device-history-printer" title={job.name}>{job.name}</span>}<time dateTime={job.started}>{when(job.started)}</time>
    <span className="status-text" data-tone={outcomeTone(job, running)} title={reason || undefined}>{outcomeLabel(job, running)}</span>
    {(job.error || reason) && <p className="device-error">{reason || [job.error, job.recovery].filter(Boolean).join('. ')}</p>}
  </li>;
}

function AllJobs({ runningId, version, collapse }: { runningId?: string; version: string; collapse(): void }) {
  const [pages, setPages] = useState<{ sends: History[]; total: number }>({ sends: [], total: 0 }), [count, setCount] = useState(HISTORY_PAGE), [error, setError] = useState('');
  useEffect(() => {
    const abort = new AbortController();
    void fetch(`/api/printer-sends?limit=${count}`, { signal: abort.signal }).then(async (r) => {
      if (!r.ok) throw await responseError(r);
      const body = await r.json() as { sends: History[]; total: number }; if (!abort.signal.aborted) { setPages(body); setError(''); }
    }).catch((e: unknown) => { if (!abort.signal.aborted) setError(e instanceof Error ? e.message : 'Couldn\'t load print history'); });
    return () => abort.abort();
  }, [count, version]);
  return <>{error && <p role="status" className="device-error">{error}</p>}
    <ol className="device-history" data-printers="true">{pages.sends.map((job) => <HistoryRow key={job.id} job={job} running={job.id === runningId} printer />)}</ol>
    <div className="device-history-more">{pages.sends.length < pages.total && <button type="button" className="link" onClick={() => setCount((n) => n + HISTORY_PAGE)}>Show more</button>}
      <button type="button" className="link" onClick={collapse}>Show less</button><span>{pages.sends.length} of {pages.total}</span></div></>;
}

function DevicePanel({ connection, model, cover, status, live }: { connection: PrinterConnection; model: string; cover?: string; status?: PrinterStatus; live: boolean }) {
  const [thumbnailFailed, failThumbnail] = useState(false);
  const [error, setError] = useState(''), [pending, setPending] = useState(false), [history, setHistory] = useState<History[]>([]), [historyError, setHistoryError] = useState(''), [historyLoaded, setHistoryLoaded] = useState(false);
  const transfers = useStore((s) => s.printerConnections.transfers), [allJobs, showAllJobs] = useState(false);
  const historyVersion = transfers.filter((t) => t.printer === connection.printer).map((t) => `${t.id}:${t.state}:${t.outcome}`).join(',');
  useEffect(() => {
    const abort = new AbortController();
    setHistoryLoaded(false);
    void fetch(`/api/printer-sends?printer=${encodeURIComponent(connection.printer)}`, { signal: abort.signal }).then(async (r) => {
      if (!r.ok) throw await responseError(r);
      const body = await r.json() as { sends: History[] }; if (!abort.signal.aborted) { setHistory(body.sends); setHistoryError(''); setHistoryLoaded(true); }
    }).catch((e: unknown) => { if (!abort.signal.aborted) { setHistoryError(e instanceof Error ? e.message : 'Couldn\'t load recent prints'); setHistoryLoaded(true); } });
    return () => abort.abort();
  }, [connection.printer, historyVersion]);
  useEffect(() => failThumbnail(false), [status?.job?.id]);
  const act = async (work: () => Promise<void>) => {
    setPending(true); setError('');
    try { await work(); return true; } catch (e) { setError(e instanceof Error ? e.message : 'Oops! The printer didn\'t accept that command'); return false; } finally { setPending(false); }
  };
  const offline = !live || !status?.online, busy = offline || pending;
  const bambu = connection.kind === 'bambu-lan', elegoo = connection.kind === 'elegoolink' && !connection.upload_only;
  const temperaturesEditable = bambu || elegoo || ['moonraker', 'octoprint', 'duet'].includes(connection.kind);
  const capabilities = printerCapabilities(model);
  const { lightLabel } = capabilities;
  const dual = (status?.nozzle_temps?.length ?? 0) > 1;
  const active = status?.state === 'printing' || status?.state === 'paused', canStop = active || status?.state === 'busy';
  const run = (action: 'pause' | 'resume' | 'stop') => void act(async () => {
    if (action === 'stop' && !await confirmDialog('Stop print', `Stop the print on ${connection.name}? Once stopped, it can't be resumed.`, ['Stop print', 'Cancel'], true)) return;
    await controlPrinter(connection.printer, action);
  });
  const fans = bambu ? Object.entries(status?.fans ?? {}).filter(([name]) => status?.controls?.fans?.[name] && capabilities.fans.includes(name)).sort(([a], [b]) => ['part', 'auxiliary', 'chamber'].indexOf(a) - ['part', 'auxiliary', 'chamber'].indexOf(b)) : [];
  const lightCtl = useDeviceControl(connection.printer, { control: 'light' }, (bambu || elegoo) && status?.light != null ? Number(status.light) : undefined);
  const speedCtl = useDeviceControl(connection.printer, { control: 'speed' }, bambu ? status?.speed_mode : undefined);
  const light = lightCtl.value === undefined ? undefined : lightCtl.value === 1, speed = speedCtl.value;
  const trays = status?.trays ?? [], units = new Map<number, Tray[]>();
  const reports = usePrinterSlots(bambu ? connection.printer : undefined);
  const stored = useStore((s) => setupOf(s, connection.printer));
  const reportOf = (t: Tray) => reports.slots?.slots.find((x) => x.slot === slotOf(t));
  const chosenOf = (t: Tray) => stored?.slots.find((x) => x.tray === slotOf(t))?.filament ?? '';
  const swatchOf = (t: Tray) => { const slot = stored?.slots.findIndex((x) => x.tray === slotOf(t)) ?? -1; return slot >= 0 ? { printer: connection.printer, slot } : undefined; };
  const filaments = useStore((s) => s.records.filaments), reads = useStore((s) => s.catalogReads);
  const choices = useMemo(() => filaments.filter((f) => !f.archived).map((f) => ({ key: f.id, name: f.name, type: profileInfo(getState(), 'filament', f.id).entry?.type ?? '' })).sort((a, b) => a.name.localeCompare(b.name)), [filaments, reads]); // eslint-disable-line react-hooks/exhaustive-deps
  const [choosing, setChoosing] = useState(false);
  const choose = (t: Tray) => (filament: string) => { setChoosing(true); act(() => pickSlotFilament(connection.printer, slotOf(t), filament, reportedUnits(trays, stored?.nozzles.length ?? 1), !!reportOf(t)?.spool)).finally(() => setChoosing(false)); };
  for (const tray of trays.filter((t) => t.kind !== 'external')) units.set(tray.ams, [...(units.get(tray.ams) ?? []), tray]);
  const lastJob = history.find((job) => job.state === 'done' && job.start);
  const runningId = canStop && status?.file ? history.find((job) => job.start && job.outcome === 'printing' && !job.job_end && job.filename === status.file)?.id : undefined;
  const filename = status?.file || (!canStop ? lastJob?.filename : undefined);
  const end = jobEnd(status);
  const eta = canStop && status?.remaining_s != null ? clock(new Date(status.at).getTime() + status.remaining_s * 1000) : undefined;
  const temperatureRows = <>{status?.nozzle_temps?.length ? status.nozzle_temps.map((t) => <TemperatureRow key={t.id} label={dual ? `${t.id === 0 ? 'Right' : 'Left'} nozzle` : 'Nozzle'} actual={t.actual} target={t.target} max={350} editable={bambu} offline={offline} printer={connection.printer} command={{ control: 'nozzle', extruder: t.id }} />)
    : status?.nozzle && <TemperatureRow label="Nozzle" actual={status.nozzle.actual} target={status.nozzle.target} max={300} editable={temperaturesEditable && (status.nozzles?.length ?? 0) <= 1} offline={offline} printer={connection.printer} command={{ control: 'nozzle' }} />}
    {status?.bed && <TemperatureRow label="Bed" actual={status.bed.actual} target={status.bed.target} max={120} editable={temperaturesEditable} offline={offline} printer={connection.printer} command={{ control: 'bed' }} />}
    {capabilities.chamber && status?.chamber && <TemperatureRow label="Chamber" actual={status.chamber.actual} target={status.controls?.chamber ? status.chamber.target : undefined} min={status.controls?.chamber?.min} max={status.controls?.chamber?.max ?? 60} editable={bambu && !!status.controls?.chamber} offline={offline} printer={connection.printer} command={{ control: 'chamber' }} />}</>;
  const phrase = !live ? 'Reconnecting…' : canStop ? status?.state === 'paused' ? 'Paused' : status?.state === 'busy' ? 'Printer busy' : 'Printing' : end.label;
  const tone = !live ? 'offline' : canStop ? status?.state === 'busy' ? undefined : 'printing' : end.tone;
  const moonraker = connection.kind === 'moonraker', jobDuration = moonraker ? preciseDuration : duration;
  const progress = canStop ? status?.progress : status?.state === 'finished' && filename ? 1 : undefined;
  const ams = bambu && trays.length > 0 && <Card title="AMS & filament" className="device-ams">{[...units].sort(([a], [b]) => a - b).map(([id, slots]) => <div className="device-ams-unit" key={id}>{units.size > 1 && <h3>AMS {id + 1}</h3>}<div className="device-slots">{slots.sort((a, b) => a.slot - b.slot).map((tray) => <Spool key={tray.id} printer={connection.printer} tray={tray} trays={trays} report={reportOf(tray)} chosen={chosenOf(tray)} swatch={swatchOf(tray)} choices={choices} busy={busy || choosing} choose={choose(tray)} />)}</div></div>)}
          {trays.filter((t) => t.kind === 'external' && !(feedOf(model)?.external === 'exclusive' && units.size > 0)).map((tray) => <div className="device-external" key={tray.id}><Spool printer={connection.printer} tray={tray} trays={trays} report={reportOf(tray)} chosen={chosenOf(tray)} swatch={swatchOf(tray)} choices={choices} busy={busy || choosing} choose={choose(tray)} /></div>)}
        </Card>;
  const temperatures = !!(status?.nozzle || status?.nozzle_temps?.length || status?.bed || (capabilities.chamber && status?.chamber));
  const waiting = status === undefined;
  const skelRows = (labels: string[]) => labels.map((label) => <div key={label} className="device-row"><span className="device-row-label">{label}</span><span className="device-row-value"><Skeleton width={150} height={12}/></span></div>);
  const amsSkeleton = bambu && waiting && <Card title="AMS & filament" className="device-ams"><div className="device-slots" role="status" aria-busy="true" aria-label="Checking the AMS…">
    {[0, 1, 2, 3].map((i) => <div key={i} className="device-spool"><Skeleton block width={24} height={24} style={{ borderRadius: '50%', marginTop: 2 }}/><span className="device-spool-text"><Skeleton width={48} height={10} style={{ margin: '3px 0' }}/><Skeleton block width="100%" height={28} style={{ margin: '4px 0' }}/></span></div>)}</div></Card>;
  const historySkeleton = !historyLoaded && <Card title="Recent jobs" className="device-recent"><ol className="device-history" role="status" aria-busy="true" aria-label="Fetching recent jobs…">
    {[0, 1, 2].map((i) => <li key={i}><strong><Skeleton width="40%" height={12}/></strong><time><Skeleton width={90} height={10}/></time><span><Skeleton width={56} height={10}/></span></li>)}</ol></Card>;
  return <article className="device-panel" aria-label={connection.name}>
    <RecordProblem kind="machine" id={connection.printer} className="device-problem"/>
    {error && <p role="alert" className="device-command-error">{error}</p>}
    <div className="device-layout">
      <div className="device-left"><DeviceCamera printer={connection.printer} cover={cover} light={light} lightLabel={lightLabel} cameraLabel={capabilities.cameraLabel} busy={offline} onLight={() => lightCtl.commit(light ? 0 : 1)} />
        {ams || amsSkeleton || <SlotsCard printer={connection.printer} live={live}/>}
        <NozzlesCard printer={connection.printer}/>
      </div>
      <div className="device-status">
        <section className="device-job" aria-label="Print job">
          {status?.job?.thumbnail && !thumbnailFailed && <div className="device-job-art"><img src={status.job.thumbnail} alt={`Plate ${status.job.plate}`} onError={() => failThumbnail(true)} /></div>}
          <div className="device-job-content">
            <strong className="device-job-name">{waiting ? <Skeleton width={'45%'} height={16} style={{ margin: '3px 0' }}/> : filename ? jobName(filename) : 'No active print'}</strong>
            <span className="device-job-state">{status?.job && filename && <span>Plate {status.job.plate}</span>}<span className="status-text" data-tone={tone}>{phrase}</span>{!canStop && live && end.reason && <span>{end.reason}</span>}</span>
            {progress != null && <div className="device-job-progress"><progress max="1" value={progress} aria-label="Print progress" /><strong>{moonraker ? Math.floor(progress * 100) : Math.round(progress * 100)}<small>%</small></strong></div>}
            {canStop && moonraker && <div className="device-job-meta">
              {status?.elapsed_s != null && <span>Elapsed {preciseDuration(status.elapsed_s)}</span>}
              <span>File estimate {status?.file_estimate_s != null ? preciseDuration(status.file_estimate_s) : '–'}</span>
              <span>Slicer estimate {status?.slicer_estimate_s != null ? preciseDuration(status.slicer_estimate_s) : '–'}</span>
            </div>}
            {canStop && <div className="device-job-meta">{status?.layer != null && status.layer > 0 && <span>Layer {status.layer}{status.layers != null && ` / ${status.layers}`}</span>}{status?.remaining_s != null && <span>{jobDuration(status.remaining_s)} left</span>}{eta && <span>ETA {eta}</span>}</div>}
          </div>
          {canStop && <div className="device-job-actions">{active && <button className="btn" type="button" disabled={busy} onClick={() => run(status?.state === 'paused' ? 'resume' : 'pause')}>{status?.state === 'paused' ? 'Resume' : 'Pause'}</button>}<button className="btn btn-danger" type="button" disabled={busy} onClick={() => run('stop')}>Stop</button></div>}
        </section>
        {temperatures ? <Card title="Temperatures" className="device-temperatures">{temperatureRows}</Card>
          : waiting && <Card title="Temperatures" className="device-temperatures"><div role="status" aria-busy="true" aria-label="Checking temperatures…">{skelRows(['Nozzle', 'Bed'])}</div></Card>}
        {(fans.length > 0 || speed != null || light != null) && <Card title="Controls" className="device-controls">
          {fans.map(([name, value]) => { const capability = status!.controls!.fans![name]; return <FanRow key={name} printer={connection.printer} fan={name} label={name === 'part' ? 'Part fan' : name === 'auxiliary' ? 'Aux fan' : `${name[0].toUpperCase()}${name.slice(1)} fan`} reported={value} min={capability.min} max={capability.max} offline={offline} />; })}
          {speed != null && <ControlRow label="Speed">{() => <><Dropdown aria-label="Print speed" value={String(speed)} disabled={offline} options={SPEEDS.map((o) => ({ value: o.id, label: o.label }))} onValueChange={(value) => speedCtl.commit(Number(value))} /><ControlError error={speedCtl.error} /></>}</ControlRow>}
          {light != null && <ControlRow label={lightLabel}>{(id) => <><Check aria-labelledby={id} checked={light} disabled={offline} onCheckedChange={(on) => lightCtl.commit(on ? 1 : 0)} /><ControlError error={lightCtl.error} /></>}</ControlRow>}
        </Card>}
        {!!status?.messages?.length && <Card title={`Printer messages (${status.messages.length})`} className="device-messages"><ul>{status.messages.map((message, i) => <li key={`${message.code}:${i}`}><p>{message.message || message.code}</p>{message.message && <span>{message.code}</span>}</li>)}</ul></Card>}
      </div>
      {historySkeleton}
      {historyLoaded && (history.length > 0 || historyError) && <Card title={allJobs ? 'All jobs' : 'Recent jobs'} className="device-recent">{historyError && <p role="status" className="device-error">{historyError}</p>}
        {allJobs ? <AllJobs runningId={runningId} version={transfers.map((t) => `${t.id}:${t.state}:${t.outcome}`).join(',')} collapse={() => showAllJobs(false)} /> : <>
          <ol className="device-history">{history.slice(0, 5).map((job) => <HistoryRow key={job.id} job={job} running={job.id === runningId} />)}</ol>
          <div className="device-history-more"><button type="button" className="link" onClick={() => showAllJobs(true)}>Show all</button></div></>}</Card>}
    </div>
  </article>;
}

const openConnection = (printer: string) => openLibrary('machine', { name: printer });

function OfflinePanel({ printer, name, cover }: { printer: string; name: string; cover?: string }) {
  const enabled = useStore((s) => s.printerConnections.enabled);
  return <div className="device-panel device-offline">
    <header className="device-head"><PrinterRender cover={cover} /><div><h1>{name}</h1>
      <RecordProblem kind="machine" id={printer}/>
      <p className="device-offline-line"><span className="status-text" data-tone="offline">Not connected</span>{enabled ? <button type="button" className="btn-ghost device-connect" onClick={() => openConnection(printer)}>Connect</button>
        : <span className="device-network-off">Printer connections are switched off on this PancakeSlicer service right now.</span>}</p></div></header>
    <SlotsCard printer={printer} live={false}/>
    <NozzlesCard printer={printer}/>
  </div>;
}

export function DeviceView() {
  const connections = useStore((s) => s.printerConnections);
  const records = useStore((s) => s.records.printers);
  const platePrinter = useStore((s) => s.printer), [selected, select] = useState(platePrinter);
  const setups = records.filter((r) => !r.archived).map((r) => ({ key: r.id, name: r.name, vendor: r.vendor, model: r.model })).sort((a, b) => a.name.localeCompare(b.name));
  const printers = setups.map((setup) => ({ setup, connection: connections.connections.find((c) => c.printer === setup.key) }));
  const current = printers.find((p) => p.setup.key === selected) ?? printers[0];
  const asked = useStore((s) => s.device);
  useEffect(() => { if (asked.at && asked.printer) select(asked.printer); }, [asked.at]); // eslint-disable-line react-hooks/exhaustive-deps
  useEffect(() => { document.querySelector('.device-printer[aria-pressed=true]')?.scrollIntoView({ block: 'nearest', inline: 'nearest' }); }, [current?.setup.key]);
  return <main className="device-view" aria-label="Device">
    <aside className="device-list" aria-label="Printers"><h2>Printers</h2><div className="device-printer-rows">{printers.map(({ setup, connection }) => {
      const own = connection?.name && connection.name !== setup.name ? connection.name : connection?.serial ? `Serial ${connection.serial.slice(-4)}` : undefined;
      const cover = printerCover(setup.vendor, {}, setup.model);
      const state = connection ? presenceStatus(connections.live, connection, connections.status[connection.printer], true) : { label: 'Not connected', tone: 'offline' as const };
      return <button type="button" className="device-printer" aria-pressed={setup.key === current?.setup.key} key={setup.key} onClick={() => select(setup.key)}>
        <PrinterRender cover={cover} /><span><strong>{setup.name}</strong>{own && <span className="device-printer-own">{own}</span>}<span className="status-text" data-tone={state.tone}>{state.label}</span></span>
      </button>;
    })}{!printers.length && <p className="device-muted">No printers yet</p>}</div>
      <div className="device-list-actions">{current && connections.enabled && <button type="button" className="btn" onClick={() => openConnection(current.setup.key)}>Connection</button>}
        <button type="button" className="btn device-manage" onClick={() => openLibrary('machine')}>Manage printers</button></div></aside>
    {current?.connection ? <DevicePanel key={current.setup.key} connection={{ ...current.connection, name: current.setup.name }} model={current.setup.model || current.connection.model || ''} cover={printerCover(current.setup.vendor, {}, current.setup.model)} status={connections.status[current.setup.key]} live={connections.live} />
      : current ? <OfflinePanel key={current.setup.key} printer={current.setup.key} name={current.setup.name} cover={printerCover(current.setup.vendor, {}, current.setup.model)}/>
      : <div className="device-empty"><h1>Add a printer</h1><p>Add your printer to start slicing for it and keep all its filaments handy here.</p></div>}
  </main>;
}
