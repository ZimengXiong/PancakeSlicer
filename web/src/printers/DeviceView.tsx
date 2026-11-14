import { useEffect, useId, useMemo, useRef, useState, type ReactNode } from 'react';
import { useStore, setState, getState } from '../state/store';
import { listProfiles } from '../profiles/pickerSource';
import { printerCover } from '../profiles/metadata';
import { confirmDialog } from '../components/Ask';
import { Segmented } from '../components/kit';
import { Check } from '../components/Check';
import { NumberBox } from '../components/Field';
import { Range } from '../components/Range';
import { responseError } from '../api/access';
import { controlDevice, controlPrinter, presenceStatus, statusLine, type PrinterConnection, type PrinterStatus, type StatusTone, type Tray } from './connections';
import { printerCapabilities, printerModel } from './capabilities';
import { DeviceCamera } from './DeviceCamera';
import { chooseSlot, ruleText, slotOf, usePrinterSlots, type SlotMatch } from './slots';
import { trayCode } from '../multinozzle/ams';
import { Dropdown } from '../components/Dropdown';
import { outcomeLabel, outcomeTone } from './sendOutcome';
import type { Transfer } from '../flow/transfer';
import './printers.css';
import './device.css';

function Card({ title, action, className = '', children }: { title: string; action?: ReactNode; className?: string; children: ReactNode }) {
  return <section className={`device-card ${className}`}>{action ? <div className="device-card-head"><h2>{title}</h2>{action}</div> : <h2>{title}</h2>}{children}</section>;
}

function ControlRow({ label, children }: { label: string; children(labelId: string): ReactNode }) {
  const id = useId();
  return <div className="device-row" role="group" aria-labelledby={id}><span id={id} className="device-row-label">{label}</span>{children(id)}</div>;
}

function TemperatureRow({ label, actual, target, min = 0, max, editable, busy, apply }: { label: string; actual: number; target?: number; min?: number; max: number; editable: boolean; busy: boolean; apply(value: number): void }) {
  return <ControlRow label={label}>{(id) => <span className="device-row-value"><span className="device-row-actual">{Math.round(actual)} °C</span>
    <span className="device-row-to">Target</span>
    {editable ? <NumberBox labelledBy={id} label={`${label} target`} value={target ?? 0} min={min} max={max} unit="°C" readOnly={busy} onCommit={apply} />
      : <span>{target != null ? Math.round(target) : '–'} °C</span>}</span>}</ControlRow>;
}

function FanRow({ label, value, min, max, busy, apply }: { label: string; value: number; min: number; max: number; busy: boolean; apply(value: number): void }) {
  const [shown, show] = useState(value), timer = useRef<ReturnType<typeof setTimeout>>(undefined);
  useEffect(() => { if (!timer.current) show(value); }, [value]);
  useEffect(() => () => clearTimeout(timer.current), []);
  const send = (v: number) => { show(v); clearTimeout(timer.current); timer.current = setTimeout(() => { timer.current = undefined; apply(v); }, 300); };
  return <ControlRow label={label}>{(id) => <span className="device-row-value device-fan">
    <Range aria-label={`${label} speed`} value={shown} min={min} max={max} disabled={busy} onValueChange={show} onValueCommit={send} />
    <NumberBox labelledBy={id} label={label} value={shown} min={min} max={max} unit="%" readOnly={busy} onCommit={send} /></span>}</ControlRow>;
}

const duration = (seconds: number) => { const n = Math.max(0, Math.round(seconds / 60)); return n >= 60 ? `${Math.floor(n / 60)} h ${n % 60} min` : `${n} min`; };
const clock = (date: string | number) => new Date(date).toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' });
const when = (date: string) => {
  const d = new Date(date), days = Math.round((new Date().setHours(0, 0, 0, 0) - new Date(date).setHours(0, 0, 0, 0)) / 864e5);
  return `${days === 0 ? 'Today' : days === 1 ? 'Yesterday' : d.toLocaleDateString([], { month: 'short', day: 'numeric' })} ${clock(date)}`;
};
const jobName = (file: string) => file.replace(/\.gcode(\.3mf)?$/i, '').replace(/\.3mf$/i, '');
const SPEEDS = [{ id: '1', label: 'Silent' }, { id: '2', label: 'Standard' }, { id: '3', label: 'Sport' }, { id: '4', label: 'Ludicrous' }] as const;
type History = Transfer;
type Control = 'nozzle' | 'bed' | 'chamber' | 'fan' | 'speed' | 'light';

function Spool({ tray, trays, dual, external = false, match, choices, busy, choose }: { tray: Tray; trays: readonly Tray[]; dual: boolean; external?: boolean; match?: SlotMatch;
  choices: { key: string; name: string; type: string }[]; busy: boolean; choose(filament: string | null): void }) {
  const colour = /^#[0-9a-f]{6}$/i.test(tray.color) ? tray.color : undefined;
  const remaining = tray.type && tray.remaining != null && tray.remaining >= 0 ? Math.min(100, tray.remaining) : undefined;
  const where = trayCode(tray, trays);
  const chosen = match?.rule === 'remembered' || match?.rule === 'spool';
  const same = (c: { type: string }) => c.type.toUpperCase() === tray.type.toUpperCase();
  const options = match && [...(match.filament ? [] : [{ value: '', label: match.name }]),
    ...[...choices].sort((a, b) => Number(same(b)) - Number(same(a))).map((c) => ({ value: c.key, label: c.name, search: `${c.name} ${c.type}` })),
    ...(chosen ? [{ value: AUTO, label: 'Match automatically' }] : [])];
  return <div className="device-spool" data-active={tray.active || undefined} aria-label={`${where}, ${tray.type || 'empty'}${tray.active ? ', in use' : ''}`}>
    <span className="device-spool-disc" style={{ backgroundColor: tray.type ? colour : undefined }} data-empty={!tray.type || undefined} aria-hidden="true" />
    <span className="device-spool-text"><span className="device-spool-where">{where}{dual && tray.extruder != null && !external && ` ${tray.extruder === 0 ? 'R' : 'L'}`}{tray.active && <span className="status-text" data-tone="printing"> In use</span>}</span>
      {match && options ? <Dropdown className="combo device-spool-pick" value={match.filament ?? ''} disabled={busy} aria-label={`Filament in ${where}`} options={options}
          onValueChange={(next) => next && next !== match.filament && choose(next === AUTO ? null : next)} />
        : <span>{tray.type || 'Empty'}</span>}
      {match && <span className="device-spool-why">{ruleText(match)}{match.rule !== 'exact' && tray.type && ` (${tray.type})`}</span>}</span>
    {remaining != null && <progress aria-label={`${where} remaining`} max="100" value={remaining} />}
  </div>;
}
const AUTO = '\0auto';

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
const jobReason = (job: History) => job.job_end === 'failed' ? (job.job_errors ?? []).map((e) => e.message ? `${e.message} (${e.code})` : `Error ${e.code}`).join(' · ') : '';

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
  const [error, setError] = useState(''), [pending, setPending] = useState(false), [history, setHistory] = useState<History[]>([]), [historyError, setHistoryError] = useState('');
  const transfers = useStore((s) => s.printerConnections.transfers), [allJobs, showAllJobs] = useState(false);
  const historyVersion = transfers.filter((t) => t.printer === connection.printer).map((t) => `${t.id}:${t.state}:${t.outcome}`).join(',');
  useEffect(() => {
    const abort = new AbortController();
    void fetch(`/api/printer-sends?printer=${encodeURIComponent(connection.printer)}`, { signal: abort.signal }).then(async (r) => {
      if (!r.ok) throw await responseError(r);
      const body = await r.json() as { sends: History[] }; if (!abort.signal.aborted) { setHistory(body.sends); setHistoryError(''); }
    }).catch((e: unknown) => { if (!abort.signal.aborted) setHistoryError(e instanceof Error ? e.message : 'Couldn\'t load recent prints'); });
    return () => abort.abort();
  }, [connection.printer, historyVersion]);
  useEffect(() => failThumbnail(false), [status?.job?.id]);
  const act = async (work: () => Promise<void>) => {
    setPending(true); setError('');
    try { await work(); return true; } catch (e) { setError(e instanceof Error ? e.message : 'Oops! The printer didn\'t accept that command'); return false; } finally { setPending(false); }
  };
  const busy = !live || !status?.online || pending;
  const bambu = connection.kind === 'bambu-lan', elegoo = connection.kind === 'elegoolink' && !/2$/.test(model || connection.model || '');
  const temperaturesEditable = bambu || ['moonraker', 'octoprint', 'duet'].includes(connection.kind) || elegoo;
  const capabilities = printerCapabilities(model);
  const { lightLabel } = capabilities;
  const dual = (status?.nozzle_temps?.length ?? 0) > 1;
  const active = status?.state === 'printing' || status?.state === 'paused', canStop = active || status?.state === 'busy';
  const set = (control: Control, value: number, fan?: string, extruder?: number) => act(() => controlDevice(connection.printer, { control, value, fan, extruder }));
  const run = (action: 'pause' | 'resume' | 'stop') => void act(async () => {
    if (action === 'stop' && !await confirmDialog('Stop print', `Stop the print on ${connection.name}? Once stopped, it can't be resumed.`, ['Stop print', 'Cancel'], true)) return;
    await controlPrinter(connection.printer, action);
  });
  const fans = bambu ? Object.entries(status?.fans ?? {}).filter(([name]) => status?.controls?.fans?.[name] && capabilities.fans.includes(name)).sort(([a], [b]) => ['part', 'auxiliary', 'chamber'].indexOf(a) - ['part', 'auxiliary', 'chamber'].indexOf(b)) : [];
  const light = (bambu || elegoo) ? status?.light : undefined, speed = bambu ? status?.speed_mode : undefined;
  const trays = status?.trays ?? [], units = new Map<number, Tray[]>();
  const slotMatches = usePrinterSlots(bambu ? connection.printer : undefined);
  const matchOf = (t: Tray) => slotMatches.slots?.slots.find((m) => m.slot === slotOf(t));
  const choicesKey = useStore(() => (JSON.stringify(listProfiles('filament').map((p) => ({ key: p.key, name: p.name, type: String(Array.isArray(p.config.filament_type) ? p.config.filament_type[0] : p.config.filament_type ?? '') })))));
  const choices = useMemo(() => JSON.parse(choicesKey) as { key: string; name: string; type: string }[], [choicesKey]);
  const [choosing, setChoosing] = useState(false);
  const choose = (t: Tray) => (filament: string | null) => { setChoosing(true); act(() => chooseSlot(connection.printer, slotOf(t), filament).then(slotMatches.set)).finally(() => setChoosing(false)); };
  for (const tray of trays.filter((t) => t.kind !== 'external')) units.set(tray.ams, [...(units.get(tray.ams) ?? []), tray]);
  const lastJob = history.find((job) => job.state === 'done' && job.start);
  const runningId = canStop && status?.file ? history.find((job) => job.start && job.outcome === 'printing' && !job.job_end && job.filename === status.file)?.id : undefined;
  const filename = status?.file || (!canStop ? lastJob?.filename : undefined);
  const end = jobEnd(status);
  const platePrinter = useStore((s) => s.printer), [syncing, setSyncing] = useState(false);
  const useFilaments = () => { setSyncing(true); void import('../plater/amsSync').then((m) => m.syncAms(connection.printer)).finally(() => setSyncing(false)); };
  const eta = canStop && status?.remaining_s != null ? clock(new Date(status.at).getTime() + status.remaining_s * 1000) : undefined;
  const temperatureRows = <>{status?.nozzle_temps?.length ? status.nozzle_temps.map((t) => <TemperatureRow key={t.id} label={dual ? `${t.id === 0 ? 'Right' : 'Left'} nozzle` : 'Nozzle'} actual={t.actual} target={t.target} max={350} editable={bambu} busy={busy} apply={(v) => void set('nozzle', v, undefined, t.id)} />)
    : status?.nozzle && <TemperatureRow label="Nozzle" actual={status.nozzle.actual} target={status.nozzle.target} max={300} editable={temperaturesEditable && (status.nozzles?.length ?? 0) <= 1} busy={busy} apply={(v) => void set('nozzle', v)} />}
    {status?.bed && <TemperatureRow label="Bed" actual={status.bed.actual} target={status.bed.target} max={120} editable={temperaturesEditable} busy={busy} apply={(v) => void set('bed', v)} />}
    {capabilities.chamber && status?.chamber && <TemperatureRow label="Chamber" actual={status.chamber.actual} target={status.controls?.chamber ? status.chamber.target : undefined} min={status.controls?.chamber?.min} max={status.controls?.chamber?.max ?? 60} editable={bambu && !!status.controls?.chamber} busy={busy} apply={(v) => void set('chamber', v)} />}</>;
  const phrase = !live ? 'Reconnecting…' : canStop ? status?.state === 'paused' ? 'Paused' : status?.state === 'busy' ? 'Printer busy' : 'Printing' : end.label;
  const tone = !live ? 'offline' : canStop ? status?.state === 'busy' ? undefined : 'printing' : end.tone;
  const progress = canStop ? status?.progress : status?.state === 'finished' && filename ? 1 : undefined;
  const ams = bambu && trays.length > 0 && <Card title="AMS & filament" className="device-ams" action={platePrinter === connection.printer && <button type="button" className="btn slant device-action" disabled={syncing} onClick={useFilaments}>{syncing ? 'Reading…' : 'Use printer\'s filaments'}</button>}>{[...units].sort(([a], [b]) => a - b).map(([id, slots]) => <div className="device-ams-unit" key={id}>{units.size > 1 && <h3>AMS {id + 1}</h3>}<div className="device-slots">{slots.sort((a, b) => a.slot - b.slot).map((tray) => <Spool key={tray.id} tray={tray} trays={trays} dual={dual} match={matchOf(tray)} choices={choices} busy={busy || choosing} choose={choose(tray)} />)}</div></div>)}
          {trays.filter((t) => t.kind === 'external').map((tray) => <div className="device-external" key={tray.id}><Spool tray={tray} trays={trays} dual={dual} external match={matchOf(tray)} choices={choices} busy={busy || choosing} choose={choose(tray)} /></div>)}
        </Card>;
  const temperatures = !!(status?.nozzle || status?.nozzle_temps?.length || status?.bed || (capabilities.chamber && status?.chamber));
  return <article className="device-panel" aria-label={connection.name}>
    {error && <p role="alert" className="device-command-error">{error}</p>}
    <div className="device-layout">
      <div className="device-left"><DeviceCamera printer={connection.printer} cover={cover} light={light} lightLabel={lightLabel} cameraLabel={capabilities.cameraLabel} busy={busy} onLight={() => void set('light', light ? 0 : 1)} />
        {ams}
      </div>
      <div className="device-status">
        <section className="device-job" aria-label="Print job">
          {status?.job?.thumbnail && !thumbnailFailed && <div className="device-job-art"><img src={status.job.thumbnail} alt={`Plate ${status.job.plate}`} onError={() => failThumbnail(true)} /></div>}
          <div className="device-job-content">
            <strong className="device-job-name">{filename ? jobName(filename) : 'No active print'}</strong>
            <span className="device-job-state">{status?.job && filename && <span>Plate {status.job.plate}</span>}<span className="status-text" data-tone={tone}>{phrase}</span>{!canStop && live && end.reason && <span>{end.reason}</span>}</span>
            {progress != null && <div className="device-job-progress"><progress max="1" value={progress} aria-label="Print progress" /><strong>{Math.round(progress * 100)}<small>%</small></strong></div>}
            {canStop && <div className="device-job-meta">{status?.layer != null && status.layer > 0 && <span>Layer {status.layer}{status.layers != null && ` / ${status.layers}`}</span>}{status?.remaining_s != null && <span>{duration(status.remaining_s)} left</span>}{eta && <span>ETA {eta}</span>}</div>}
          </div>
          {canStop && <div className="device-job-actions">{active && <button className="btn slant" type="button" disabled={busy} onClick={() => run(status?.state === 'paused' ? 'resume' : 'pause')}>{status?.state === 'paused' ? 'Resume' : 'Pause'}</button>}<button className="btn btn-danger" type="button" disabled={busy} onClick={() => run('stop')}>Stop</button></div>}
        </section>
        {temperatures && <Card title="Temperatures" className="device-temperatures">{temperatureRows}</Card>}
        {(fans.length > 0 || speed != null || light != null) && <Card title="Controls" className="device-controls">
          {fans.map(([name, value]) => { const capability = status!.controls!.fans![name]; return <FanRow key={name} label={name === 'part' ? 'Part fan' : name === 'auxiliary' ? 'Aux fan' : `${name[0].toUpperCase()}${name.slice(1)} fan`} value={value} min={capability.min} max={capability.max} busy={busy} apply={(v) => void set('fan', v, name)} />; })}
          {speed != null && <ControlRow label="Speed">{() => <Segmented label="Print speed" value={String(speed)} options={SPEEDS.map((o) => ({ ...o, disabled: busy }))} onChange={(value) => void set('speed', Number(value))} />}</ControlRow>}
          {light != null && <ControlRow label={lightLabel}>{(id) => <Check aria-labelledby={id} checked={light} disabled={busy} onCheckedChange={(on) => void set('light', on ? 1 : 0)} />}</ControlRow>}
        </Card>}
        {!!status?.messages?.length && <Card title={`Printer messages (${status.messages.length})`} className="device-messages"><ul>{status.messages.map((message, i) => <li key={`${message.code}:${i}`}><p>{message.message || message.code}</p>{message.message && <span>{message.code}</span>}</li>)}</ul></Card>}
      </div>
      {(history.length > 0 || historyError) && <Card title={allJobs ? 'All jobs' : 'Recent jobs'} className="device-recent">{historyError && <p role="status" className="device-error">{historyError}</p>}
        {allJobs ? <AllJobs runningId={runningId} version={transfers.map((t) => `${t.id}:${t.state}:${t.outcome}`).join(',')} collapse={() => showAllJobs(false)} /> : <>
          <ol className="device-history">{history.slice(0, 5).map((job) => <HistoryRow key={job.id} job={job} running={job.id === runningId} />)}</ol>
          <div className="device-history-more"><button type="button" className="link" onClick={() => showAllJobs(true)}>Show all</button></div></>}</Card>}
    </div>
  </article>;
}

const openConnection = (printer: string) => setState({ presetUx: { kind: 'presets', type: 'machine', name: printer } });

export function DeviceView() {
  const connections = useStore((s) => s.printerConnections);
  useStore((s) => s.userPresets);
  const platePrinter = useStore((s) => s.printer), [selected, select] = useState(platePrinter);
  const setups = listProfiles('machine').map((setup) => ({ ...setup, model: printerModel(getState(), setup.key) }));
  const printers = connections.connections.filter((c) => c.printer && setups.some((p) => p.key === c.printer));
  const current = printers.find((c) => c.printer === selected) ?? printers[0];
  const shown = current && setups.find((p) => p.key === current.printer);
  useEffect(() => { document.querySelector('.device-printer[aria-pressed=true]')?.scrollIntoView({ block: 'nearest', inline: 'nearest' }); }, [current?.printer]);
  return <main className="device-view" aria-label="Device">
    <aside className="device-list" aria-label="Printers"><h2>Printers</h2><div className="device-printer-rows">{printers.map((connection) => {
      const setup = setups.find((p) => p.key === connection.printer)!;
      const own = connection.name && connection.name !== setup.name ? connection.name : connection.serial ? `Serial ${connection.serial.slice(-4)}` : undefined;
      const cover = printerCover(setup.vendor, setup.config, setup.model), state = presenceStatus(connections.live, connection, connections.status[connection.printer], true);
      return <button type="button" className="device-printer" aria-pressed={connection.printer === current?.printer} key={connection.printer} onClick={() => select(connection.printer)}>
        <PrinterRender cover={cover} /><span><strong>{setup.name}</strong>{own && <span className="device-printer-own">{own}</span>}<span className="status-text" data-tone={state.tone}>{state.label}</span></span>
      </button>;
    })}{!printers.length && <p className="device-muted">No printers connected</p>}</div>
      <div className="device-list-actions">{current && <button type="button" className="btn slant" onClick={() => openConnection(current.printer)}>Connection</button>}
        <button type="button" className="btn slant device-manage" onClick={() => setState({ presetUx: { kind: 'presets', type: 'machine' } })}>Manage printers</button></div></aside>
    {current && shown ? <DevicePanel key={current.printer} connection={{ ...current, name: shown.name }} model={shown.model || current.model || ''} cover={printerCover(shown.vendor, shown.config, shown.model)} status={connections.status[current.printer]} live={connections.live} /> : <div className="device-empty"><h1>Connect a printer</h1><p>Watch live progress and control your printer from here.</p><button type="button" className="btn-primary" onClick={() => setState({ appDialog: 'send' })}>Connect</button></div>}
  </main>;
}
