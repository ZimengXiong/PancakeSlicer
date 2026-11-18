import { useStore, type State } from '../state/store';
import { openDevice } from '../navigation';
import { connectionOf, presenceStatus } from './connections';
import { statusLineShown } from '../boot-shape';
import './printers.css';

export function PrinterLive() {
  const enabled = useStore((s) => s.printerConnections.enabled);
  const read = (s: State) => {
    const c = s.printerConnections, connection = connectionOf(c, s.printer);
    return connection && presenceStatus(c.live, connection, c.status[s.printer], true);
  };
  const label = useStore((s) => read(s)?.label), tone = useStore((s) => read(s)?.tone);
  const presence = label && tone ? { label, tone } : undefined;
  if (!statusLineShown(enabled)) return null;
  if (enabled === null) return <span className="printer-live" aria-hidden="true"><span className="printer-live-text status-text"><span className="skel-line" style={{ width: 40, display: 'inline-block' }}/></span></span>;
  if (!enabled) return null;
  const open = () => openDevice();
  const printing = presence?.label.startsWith('Printing') || presence?.label.startsWith('Paused');
  return (
    <button type="button" className="printer-live" onClick={open} aria-label={presence ? `${presence.label}. Open the printer on Device` : 'Offline right now — hop over to the printer on Device to connect!'}>
      <span className="printer-live-text status-text" data-tone={presence?.tone ?? 'offline'}>{presence
        ? printing ? <><span className="printer-live-wide">Online</span><span className="printer-live-narrow">{presence.label}</span></> : presence.label
        : 'Offline'}</span>
    </button>
  );
}
