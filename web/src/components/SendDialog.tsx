import { SEP } from '../separator';
import { preprintMaterialChecks } from '../multinozzle/material-checks';
import { useSlicedMapping } from '../multinozzle/sliced-mapping';
import { preprintWarnings } from '../multinozzle/preprint';
import { autoMap, availableTrays, physicalMapping, trayCode, trayFilament, type SendFilament } from '../multinozzle/ams';
import { assignedExtruder, nozzleAssignments } from '../multinozzle/assignments';
import { extruderCount, valueAt } from '../multinozzle/config';
import { useEffect, useMemo, useState, type ReactNode } from 'react';
import { SlotChip } from './SlotChip';
import { printerModel } from '../printers/capabilities';
import { Check } from './Check';
import { confirmDialog } from './Ask';
import { Dropdown } from './Dropdown';
import { Modal, ModalBody, ModalFooter, ModalTitle } from './Modal';
import { download, exportFilename, outputKind } from '../actions';
import { printerSend, usePrinterSend } from '../flow/transfer';
import { filamentColor } from '../plater/filamentColor';
import { effectiveConfig, getState, resolvePreset, toast, useStore } from '../state/store';
import { ConnectionEditor } from '../printers/ConnectionEditor';
import { slotOf, usePrinterSlots } from '../printers/slots';
import { connectionOf, connectionOutput, KIND_LABEL, presenceStatus, readTrays, type Tray } from '../printers/connections';
import { printerCapabilities } from '../printers/capabilities';
import { noticeSend, openDevice, sendOutcome } from '../printers/sendOutcome';
import '../printers/printers.css';

const sentence = (s: string) => s.charAt(0).toUpperCase() + s.slice(1);
const errorText = (e: unknown) => sentence(e instanceof Error ? e.message : String(e));
const setupName = (key: string) => resolvePreset(getState(), 'machine', key).user?.name ?? key;

function closeSend(close: () => void) {
  const { value, context } = printerSend.machine.get();
  const t = context.transfer && (getState().printerConnections.transfers.find((x) => x.id === context.transfer!.id) ?? context.transfer);
  if (t && (value === 'sending' || value === 'sent')) noticeSend(t);
  if (value === 'sent' || value === 'failed') printerSend.reset();
  close();
}

export default function SendDialog({ close: dismiss }: { close: () => void }) {
  const close = () => closeSend(dismiss);
  const plate = useStore((s) => s.printer);
  const choices = useStore((s) => {
    const base = resolvePreset(s, 'machine', s.printer).base?.name;
    return [s.printer, ...s.printerConnections.connections.filter((c) => c.printer && c.printer !== s.printer
      && resolvePreset(s, 'machine', c.printer).base?.name === base).map((c) => c.printer)].join('\n');
  }).split('\n');
  const [target, setTarget] = useState(plate);
  const printer = choices.includes(target) ? target : plate;
  const name = useStore(() => setupName(printer));
  const connection = useStore((s) => connectionOf(s.printerConnections, printer));
  const status = useStore((s) => s.printerConnections.status[printer]);
  const [editing, setEditing] = useState(false);
  const title = !connection || editing ? 'Printer connection' : 'Send to printer';
  const live = useStore((s) => s.printerConnections.live);
  const presence = connection && presenceStatus(live, connection, status);
  const head = <>
    <div className="send-target">
      {choices.length > 1
        ? <Dropdown className="combo" value={printer} aria-label="Printer" onValueChange={setTarget} options={choices.map((k) => ({ value: k, label: setupName(k) }))} />
        : <span className="send-target-name">{name}</span>}
      <span className="send-target-state status-text" data-tone={presence?.tone ?? 'offline'}>{presence?.label ?? 'Not connected'}</span>
      {connection && <button type="button" className="btn slant send-action" onClick={() => setEditing(!editing)}>{editing ? 'Done' : 'Connection'}</button>}
    </div>
    {(!connection || editing) && <ConnectionEditor key={printer} printer={printer} name={name} onSaved={() => setEditing(false)} />}
  </>;
  return (
    <Modal size="m" className="send-dialog" label={title} onClose={close}>
      <ModalTitle title={title} onClose={close} />
      {connection && !editing ? <SendView key={printer} close={close} printer={printer} head={head} edit={() => setEditing(true)} />
        : <ModalBody className="send-body">{head}</ModalBody>}
    </Modal>
  );
}

function SendView({ close, printer, head, edit }: { close: () => void; printer: string; head: ReactNode; edit(): void }) {
  const connection = useStore((s) => connectionOf(s.printerConnections, printer))!;
  const status = useStore((s) => s.printerConnections.status[printer]);
  const jobId = useStore((s) => s.job.id);
  const jobReady = useStore((s) => s.job.state === 'done' && !s.job.stale);
  const plateIndex = useStore((s) => s.plateIndex);
  const output = useStore(outputKind);
  const kind = connection.kind, bambu = kind === 'bambu-lan';
  const fits = connectionOutput(kind) === output;
  const single = useStore((s) => s.filamentSlots.length === 1);
  const machineConfig = useStore((s) => effectiveConfig(s, 'machine'));
  const resolvedModel = useStore((s) => printerModel(s, printer)) || connection.model || '';
  const elegooCC = kind === 'elegoolink' && !/2$/.test(resolvedModel) && (!resolvedModel || /^Elegoo Centauri/i.test(resolvedModel));
  const capabilities = printerCapabilities(resolvedModel);
  const [leveling, setLeveling] = useState(kind !== 'elegoolink');
  const [flowCali, setFlowCali] = useState(false);
  const [vibration, setVibration] = useState(true);
  const [timelapse, setTimelapse] = useState(false);
  const [bedType, setBedType] = useState(0);
  const filamentKey = useStore((s) => JSON.stringify(s.filamentSlots.map((name, i) => {
    const config = effectiveConfig(s, 'filament', i), base = resolvePreset(s, 'filament', name).base;
    return { key: name, type: valueAt(config.filament_type, 0), color: filamentColor(s, i + 1), name: (base?.name ?? name).replace(/\s+@.*$/, ''),
      vendor: valueAt(config.filament_vendor, 0), id: base?.filament_id, requiredHRC: Number(valueAt(config.required_nozzle_HRC, 0, '0')),
      printable: Number(valueAt(config.filament_printable, 0, '3')), compatibility: Number(valueAt(config.filament_extruder_compatibility, 0, '0')),
      ...(extruderCount(effectiveConfig(s, 'machine')) > 1 && String(nozzleAssignments(s).filament_map_mode ?? '').includes('Manual') ? { extruder: assignedExtruder(s, i) } : {}) };
  })));
  const multiNozzle = extruderCount(machineConfig) > 1;
  const sliced = useSlicedMapping(jobId, bambu && jobReady);
  const filaments = useMemo(() => (JSON.parse(filamentKey) as SendFilament[]).map((filament, i) => {
    const nozzle = sliced?.mapping?.filament_map[i];
    const material = sliced?.mapping?.materials.find((row) => row.slot === i + 1);
    return { ...filament, ...material, hasFilamentSwitch: sliced?.mapping?.has_filament_switch, ...(multiNozzle && nozzle && nozzle > 0 ? { extruder: nozzle - 1 } : {}) };
  }), [filamentKey, sliced?.mapping, multiNozzle]);
  const liveTrays = status?.online ? status.trays : undefined;
  const [readTraysList, setReadTrays] = useState<Tray[] | null>(null);
  const trays = liveTrays ?? readTraysList;
  const [amsError, setAmsError] = useState('');
  const [mapping, setMapping] = useState<number[] | null>(null);
  const slotMatches = usePrinterSlots(bambu ? printer : undefined);
  const matchOf = (t: Tray) => slotMatches.slots?.slots.find((m) => m.slot === slotOf(t));
  const shown = mapping && mapping.length === filaments.length ? mapping : trays ? autoMap(filaments, trays, (t) => matchOf(t)?.filament) : [];
  const sendState = usePrinterSend();
  const { transfer, error } = sendState.context;
  const busy = sendState.value === 'connecting' || sendState.value === 'sending';
  const [working, setWorking] = useState('');

  useEffect(() => {
    const current = printerSend.machine.get().context;
    if (current.jobId && (current.jobId !== jobId || current.target !== printer) && printerSend.machine.get().value !== 'sending') printerSend.reset();
  }, [jobId, printer]);
  useEffect(() => setMapping(null), [printer]);
  useEffect(() => {
    setReadTrays(null); setAmsError('');
    if (!bambu || liveTrays) return;
    let live = true;
    readTrays(printer).then((list) => { if (live) setReadTrays(list); }, (e) => { if (live) setAmsError(errorText(e)); });
    return () => { live = false; };
  }, [bambu, printer, !!liveTrays]);

  const materials = bambu ? preprintMaterialChecks(machineConfig, status, filaments, shown, trays ?? []) : [];
  const warnings = bambu ? [...preprintWarnings(machineConfig, status, filaments, shown, trays ?? []), ...materials.filter((row) => !row.blocked).map((row) => row.message)] : [];
  const materialBlocks = materials.filter((row) => row.blocked);
  const send = async (start: boolean) => {
    if (!jobId) return;
    if (start && warnings.length && !await confirmDialog('Check printer setup', warnings.join('\n'), ['Print anyway', 'Cancel'])) return;
    const options = bambu
      ? { bed_leveling: leveling, flow_cali: flowCali, vibration_cali: vibration, timelapse: capabilities.timelapse && timelapse, ...(trays && shown.length === filaments.length && shown.every((id) => id >= 0) ? { ams_mapping: shown, ams_mapping_2: physicalMapping(shown, trays) } : {}) }
      : elegooCC
        ? { bed_leveling: leveling, timelapse, bed_type: bedType }
        : {};
    await printerSend.submit(printer, jobId, { start, filename: exportFilename(), ...options });
  };
  const handOff = async () => {
    if (!jobId) return;
    setWorking('connect');
    try { await window.orcaDesktop!.bambuConnect!(jobId, exportFilename()); toast('Opened in Bambu Connect'); }
    catch (e) { toast(errorText(e)); } finally { setWorking(''); }
  };

  const mine = transfer && transfer.printer === printer && transfer.job === jobId ? transfer : null;
  const shownTransfer = useStore((s) => (mine && s.printerConnections.transfers.find((t) => t.id === mine.id)) || mine);
  const outcome = shownTransfer ? sendOutcome(shownTransfer) : busy ? { tone: 'progress' as const, title: `Sending to ${connection.name}`, detail: 'Connecting to the printer…' } : null;
  const running = status?.online && (status.state === 'printing' || status.state === 'paused' || status.state === 'busy');
  const blocked = !jobId || !jobReady ? 'Slice your plate first before sending it to the printer.' : !fits ? `${KIND_LABEL[kind]} takes .${connectionOutput(kind)} files; this printer profile exports .${output}.` : '';
  const needsNozzleMap = bambu && multiNozzle && (!sliced?.mapping || sliced.mapping.filament_map.length !== filaments.length);
  const unmapped = bambu && trays ? filaments.flatMap((f, i) => availableTrays(f, trays).some((t) => t.id === shown[i]) ? []
    : [`Filament ${i + 1} (${f.type}): ${availableTrays(f, trays).some((t) => t.type.toUpperCase() === f.type.toUpperCase()) ? 'choose a tray' : `no ${f.type} loaded${f.extruder !== undefined ? ' on its nozzle' : ''}; load it or choose another tray`}.`]) : [];
  const needsMap = needsNozzleMap || bambu && (trays ? unmapped.length > 0 : !single || multiNozzle);
  const [syncing, setSyncing] = useState(false);
  const syncFromPrinter = () => { setSyncing(true); void import('../plater/amsSync').then((m) => m.syncAms(printer)).finally(() => setSyncing(false)); };
  const file = <p className="send-file" title={exportFilename()}><span>{exportFilename()}</span><span>Plate {plateIndex}</span></p>;

  if (outcome && (busy || sendState.value === 'sent' || mine?.state === 'failed' || mine?.state === 'cancelled')) {
    const t = shownTransfer, settled = !busy, failedSend = outcome.tone === 'error';
    const jobLine = settled && t?.outcome === 'printing' && status?.online ? status.state === 'busy' ? 'Preparing to print…'
      : status.state === 'printing' || status.state === 'paused' ? [status.state === 'paused' ? 'Paused' : '', status.progress != null ? `${Math.round(status.progress * 100)}%` : '',
        status.remaining_s ? `${Math.round(status.remaining_s / 60)} min left` : ''].filter(Boolean).join(SEP) : '' : '';
    const bar = !t ? 0 : t.state === 'uploading' ? t.sent / Math.max(1, t.total) : t.state === 'starting' ? 1
      : jobLine && status?.progress != null && (status.state === 'printing' || status.state === 'paused') ? status.progress : null;
    const meter = busy ? outcome.detail : jobLine;
    return <>
      <ModalBody className="send-body send-state">
        <div className="send-state-text" role={failedSend ? 'alert' : 'status'}>
          <h3 data-tone={outcome.tone}>{outcome.title}</h3>
          {file}
          {!busy && outcome.detail && <p>{outcome.detail}</p>}
          {failedSend && t?.recovery && <p className="send-recovery">{t.recovery}</p>}
        </div>
        {(bar != null || meter) && <div className="send-meter">
          {bar != null && <div className="progress"><div className="progress-fill" style={{ width: `${100 * bar}%` }} /></div>}
          {meter && <span>{meter}</span>}
        </div>}
      </ModalBody>
      <ModalFooter>
        {busy && t?.state !== 'starting' ? <button type="button" className="btn" disabled={t?.state !== 'uploading'} onClick={() => printerSend.cancel()}>Cancel upload</button> : busy ? <button type="button" className="btn-primary" onClick={close}>Done</button> : failedSend ? <>
          {t?.reason === 'credentials' && <button type="button" className="btn" onClick={() => { printerSend.reset(); edit(); }}>Connection settings</button>}
          <button type="button" className="btn-primary" disabled={!!blocked} onClick={() => void send(!!t?.start)}>Retry</button>
        </> : <>
          <button type="button" className="btn" onClick={() => { close(); openDevice(); }}>Open in Device</button>
          <button type="button" className="btn-primary" onClick={close}>Done</button>
        </>}
      </ModalFooter>
    </>;
  }
  return <>
    <ModalBody className="send-body">
      {head}
      {file}
      {jobReady && needsNozzleMap && <div className={sliced?.error ? 'warning-text' : 'send-meta'} role={sliced?.error ? 'alert' : 'status'}>{sliced?.error ?? 'Checking nozzle settings from slice…'}</div>}
      {blocked && <div className="warning-text">{blocked}</div>}
      {bambu && fits && <div className="send-options" role="group" aria-label="Print options">
        <label className="modal-check"><Check checked={leveling} disabled={busy} onCheckedChange={setLeveling} />Bed leveling</label>
        <label className="modal-check"><Check checked={flowCali} disabled={busy} onCheckedChange={setFlowCali} />Flow calibration</label>
        <label className="modal-check"><Check checked={vibration} disabled={busy} onCheckedChange={setVibration} />Vibration calibration</label>
        {capabilities.timelapse && <label className="modal-check"><Check checked={timelapse} disabled={busy} onCheckedChange={setTimelapse} />Timelapse</label>}
      </div>}
      {elegooCC && fits && <div className="send-options" role="group" aria-label="Print options">
        <label className="modal-check"><Check checked={leveling} disabled={busy} onCheckedChange={setLeveling} />Heated bed leveling</label>
        <label className="modal-check"><Check checked={timelapse} disabled={busy} onCheckedChange={setTimelapse} />Timelapse</label>
        <label className="form-row"><span>Build plate</span>
          <Dropdown className="combo" value={bedType} disabled={busy} aria-label="Build plate"
            onValueChange={(next) => setBedType(Number(next))}
            options={[{ value: 0, label: 'Side A (Textured)' }, { value: 1, label: 'Side B (Smooth)' }]} />
        </label>
      </div>}
      {bambu && fits && (
        <section className="send-section" aria-label="Filaments"><div className="send-section-head"><h3>Filaments</h3>
          <button type="button" className="btn slant send-action" disabled={busy || syncing || !trays?.length} onClick={syncFromPrinter}>{syncing ? 'Checking printer…' : 'Match from printer'}</button></div>
          {trays ? <div className="ams-map">{filaments.map((f, i) => {
            const t = trays.find((x) => x.id === shown[i]);
            return (
              <div key={i} className="send-map-row">
                <span className="send-project-filament"><SlotChip n={i + 1} colour={f.color} /><span title={f.name}>{f.name || f.type}</span></span>
                <span className="send-tray-select" data-colour={!!t || undefined} data-missing={!t || undefined}>
                  <Dropdown className="combo" value={shown[i] ?? -1} disabled={busy} aria-label={`Tray for filament ${i + 1}`}
                    onValueChange={(next) => setMapping(shown.map((m, k) => (k === i ? Number(next) : m)))} options={[{ value: -1, label: 'Choose a tray', disabled: true }, ...availableTrays(f, trays).map((x) => ({ value: x.id, label: <><span className="ams-swatch tray-dot" style={{ background: x.color }} />{trayCode(x, trays)}<span className="sep">{SEP}</span><span className="tray-filament">{matchOf(x)?.name ?? trayFilament(x)}</span></> }))]} />
                  {t && <span className="ams-swatch" aria-label={`${trayCode(t, trays)} colour ${t.color}`} role="img" style={{ background: t.color }} />}
                </span>
              </div>
            );
          })}</div> : <p className={amsError ? 'warning-text' : 'hint-text'}>{amsError ? `Couldn't read AMS: ${amsError}` : 'Checking printer\'s AMS…'}</p>}
          {unmapped.map((line) => <p key={line} className="warning-text">{line}</p>)}
        </section>
      )}
      {materialBlocks.map((row) => <div key={row.message} className="warning-text" role="alert">{row.message}</div>)}
      {warnings.map((warning) => <div key={warning} className="warning-text">{warning}</div>)}
      {running && <p className="send-note" role="status">{connection.name} is {status!.state === 'paused' ? 'paused mid-print' : 'printing'}. You can start a new print once it finishes, or use Upload only right now.</p>}
      {error && <div role="alert" className="warning-text">{sentence(error)}</div>}
    </ModalBody>
    <ModalFooter>
      {sendState.value === 'failed' && <button type="button" className="btn" onClick={() => void printerSend.retry()}>Reconnect</button>}
      {kind === 'bambu-connect' ? (window.orcaDesktop?.bambuConnect
        ? <button type="button" className="btn-primary" disabled={!!blocked || !!working} onClick={() => void handOff()}>Open in Bambu Connect</button>
        : <button type="button" className="btn-primary" disabled={!!blocked} onClick={() => void download()}>Download .gcode.3mf</button>) : <>
        <button type="button" className="btn" disabled={busy || !!blocked} onClick={() => void send(false)}>Upload only</button>
        <button type="button" className="btn-primary" disabled={busy || !!blocked || needsMap || running || !!materialBlocks.length} onClick={() => void send(true)}>Print</button>
      </>}
    </ModalFooter>
  </>;
}
