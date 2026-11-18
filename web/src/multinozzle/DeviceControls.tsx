import { useEffect, useId, useState } from 'react';
import { controlDevice, type PrinterStatus } from '../printers/connections';
import { toast } from '../state/store';

export function DeviceControls({ printer, status }: { printer: string; status?: PrinterStatus }) {
  if (!status?.online || !status.controls || !status.controls.chamber && !Object.keys(status.controls.fans ?? {}).length) return null;
  return <div className="send-live" aria-label="Printer controls">
    {Object.entries(status.controls.fans ?? {}).map(([fan, range]) => <DeviceValue key={fan} printer={printer} control="fan" fan={fan}
      label={`${fan.charAt(0).toUpperCase() + fan.slice(1)} fan`} actual={status.fans?.[fan] ?? 0} unit="%" {...range} />)}
    {status.controls.chamber && <DeviceValue printer={printer} control="chamber" label="Chamber target" actual={status.chamber?.target ?? 0} unit="°C" {...status.controls.chamber} />}
  </div>;
}

function DeviceValue({ printer, control, fan, label, actual, unit, min, max }: {
  printer: string; control: 'fan' | 'chamber'; fan?: string; label: string; actual: number; unit: string; min: number; max: number;
}) {
  const id = useId(), [text, setText] = useState(String(actual)), [pending, setPending] = useState(false);
  useEffect(() => setText(String(actual)), [actual]);
  const value = Number(text), valid = text.trim() !== '' && Number.isInteger(value) && value >= min && value <= max;
  return <form className="form-row" onSubmit={async (event) => {
    event.preventDefault();
    if (!valid || pending) return;
    setPending(true);
    try { await controlDevice(printer, { control, fan, value }); }
    catch (error) { toast('devicecontrols_failed', { detail: error instanceof Error ? error.message : String(error) }); }
    finally { setPending(false); }
  }}>
    <label htmlFor={id}>{label}</label><span className="opt-field">
      <input id={id} className="text num" type="number" min={min} max={max} step="1" value={text} disabled={pending} onChange={(event) => setText(event.target.value)} />
      <span className="opt-side">{unit}</span><button type="submit" className="btn" disabled={pending || !valid || value === actual}>{pending ? 'Setting…' : 'Set'}</button>
    </span>
  </form>;
}
