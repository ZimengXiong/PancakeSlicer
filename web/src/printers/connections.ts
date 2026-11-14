import { mutation, responseError } from '../api/access';
import { getState, setState } from '../state/store';
import { printerCapabilities } from './capabilities';
import type { Transfer } from '../flow/transfer';
import { joinParts, SEP } from '../separator';

export type ConnectionKind = 'bambu-lan' | 'bambu-connect' | 'moonraker' | 'octoprint' | 'prusalink' | 'duet' | 'flashforge';
export interface PrinterConnection {
  printer: string; id: string; name: string; kind: ConnectionKind;
  host?: string; serial?: string; model?: string; url?: string; auth?: string; username?: string;
  has_secret: boolean; trust?: string;
}
export interface Temp { actual: number; target: number }
export interface NozzleStatus { id: number; extruder: number; diameter: number; type?: string; flow?: string; rack?: boolean; abnormal?: boolean; legacy?: boolean }
export interface Tray { kind?: string; extruder?: number; id: number; ams: number; slot: number; type: string; color: string; name?: string; filament_id?: string; remaining?: number; active?: boolean }
export interface PrinterStatus {
  printer: string; online: boolean;
  state: 'connecting' | 'idle' | 'printing' | 'paused' | 'finished' | 'failed' | 'busy' | 'error' | 'offline';
  job?: { id: string; plate: number; sent_at: string; thumbnail?: string }; started_at?: string; speed_mode?: number; light?: boolean; messages?: { code: string; message?: string }[];
  detail?: string; file?: string; progress?: number; remaining_s?: number; layer?: number; layers?: number;
  nozzle_temps?: (Temp & { id: number })[]; nozzle?: Temp; bed?: Temp; chamber?: Temp; nozzles?: NozzleStatus[]; fans?: Record<string, number>; trays?: Tray[]; at: string;
  controls?: { fans?: Record<string, { index: number; min: number; max: number; modern?: boolean }>; chamber?: { min: number; max: number } };
}
export interface PrinterConnections {
  enabled: boolean | null;
  live: boolean;
  connections: PrinterConnection[];
  status: Record<string, PrinterStatus>;
  transfers: Transfer[];
}

export const KIND_LABEL: Record<ConnectionKind, string> = {
  'bambu-lan': 'Bambu Lab (LAN)', 'bambu-connect': 'Bambu Connect', moonraker: 'Klipper (Moonraker)',
  octoprint: 'OctoPrint', prusalink: 'PrusaLink', duet: 'Duet (RepRapFirmware)', flashforge: 'Flashforge (LAN)',
};
export const connectionOutput = (kind: ConnectionKind) => (kind === 'bambu-lan' || kind === 'bambu-connect' ? 'gcode.3mf' : 'gcode');

export const connectionOf = (c: PrinterConnections, printer: string) => c.connections.find((x) => x.printer === printer);
export const unassigned = (c: PrinterConnections) => c.connections.filter((x) => !x.printer);

let source: EventSource | null = null;
export function followConnections() {
  if (import.meta.env.MODE === 'demo') {
    setState({ printerConnections: { enabled: true, live: true, connections: [], status: {}, transfers: [] } });
    return;
  }
  if (source || getState().api?.mock) return;
  source = new EventSource('/api/printer-connections/events');
  source.addEventListener('state', (e) => {
    const snap = JSON.parse((e as MessageEvent).data) as Omit<PrinterConnections, 'live' | 'status'> & { status: PrinterStatus[] };
    setState({ printerConnections: { enabled: snap.enabled, live: true, connections: snap.connections, transfers: snap.transfers,
      status: Object.fromEntries(snap.status.map((s) => [s.printer, s])) } });
  });
  source.onerror = () => {
    const c = getState().printerConnections;
    if (c.live) setState({ printerConnections: { ...c, live: false } });
  };
}

const json = (method: string, body?: object): RequestInit => ({ method, headers: { 'Content-Type': 'application/json' }, body: body && JSON.stringify(body) });
const path = (printer: string, rest = '') => `/api/printer-connections/${encodeURIComponent(printer)}${rest}`;
async function call<T>(url: string, init: RequestInit, quiet = false): Promise<T> {
  const r = await mutation(url, init, false, undefined, undefined, quiet);
  if (!r.ok) throw await responseError(r);
  return (r.status === 204 ? undefined : await r.json()) as T;
}

export interface ConnectionEdit {
  kind: ConnectionKind; name: string; host?: string; serial?: string; access_code?: string; model?: string;
  url?: string; auth?: string; username?: string; key?: string; clear_key?: boolean;
}
export interface ConnectionTest { ok: boolean; stage: string; message: string; model?: string }
export interface Discovered {
  kind: ConnectionKind; host?: string; url?: string; serial?: string; name?: string; model?: string;
  preset?: string; connection?: string; source: string; assigned?: string;
}

export const saveConnection = (printer: string, edit: ConnectionEdit) => call<PrinterConnection>(path(printer), json('PUT', edit));
export const removeConnection = (printer: string) => call<void>(path(printer), json('DELETE'));
export const adoptConnection = (printer: string, id: string, name: string) => call<PrinterConnection>(path(printer, '/adopt'), json('POST', { id, name }));
export const testConnection = (printer: string, edit: ConnectionEdit) => call<ConnectionTest>('/api/printer-connections/test', json('POST', { ...edit, printer }));
export const discoverPrinters = () => call<{ printers: Discovered[]; notes: string[] }>('/api/printer-connections/discover', json('POST'));
export const controlPrinter = (printer: string, action: 'stop' | 'pause' | 'resume') => call<void>(path(printer, '/control'), json('POST', { action }));
export const controlDevice = (printer: string, command: { control: 'fan' | 'chamber' | 'nozzle' | 'bed' | 'speed' | 'light'; fan?: string; extruder?: number; value: number }) => call<void>(path(printer, '/device'), json('POST', command));
export const readTrays = (printer: string) => call<{ trays: Tray[] }>(path(printer, '/ams'), json('POST'), true).then((r) => r.trays ?? []);

export const LEGACY_HOST_KEYS = ['host_type', 'print_host', 'printhost_apikey', 'printhost_port', 'printhost_user', 'printhost_password', 'printhost_authorization_type', 'flashforge_serial_number'];
export async function migrateLegacyHost(printer: string, name: string, config: Record<string, unknown>): Promise<string | null> {
  const legacy = Object.fromEntries(LEGACY_HOST_KEYS.flatMap((k) => (config[k] == null || config[k] === '' ? [] : [[k, String([config[k]].flat()[0])]])));
  if (!legacy.print_host) return null;
  const r = await mutation(path(printer, '/legacy'), json('POST', { name, config: legacy }));
  if (r.ok) return null;
  return `${name}: ${(await responseError(r)).message}`;
}

export function statusLine(st: PrinterStatus | undefined): string {
  if (!st) return '';
  if (!st.online) return st.state === 'connecting' ? 'Connecting…' : 'Offline';
  const word = { idle: 'Connected', printing: 'Printing', paused: 'Paused', finished: 'Finished', failed: 'Last print failed', busy: 'Busy', error: 'Error', offline: 'Printer offline', connecting: 'Connecting…' }[st.state];
  const parts = [word + (st.progress != null && (st.state === 'printing' || st.state === 'paused') ? ` ${Math.round(st.progress * 100)}%` : '')];
  if (st.remaining_s && st.state === 'printing') {
    const m = Math.round(st.remaining_s / 60);
    parts.push(m >= 60 ? `${Math.floor(m / 60)} h ${m % 60} min left` : `${m} min left`);
  }
  if ((st.state === 'error' || st.state === 'failed') && st.detail && st.detail.toLowerCase() !== st.state) parts.push(st.detail);
  return joinParts(parts);
}
export type StatusTone = 'online' | 'printing' | 'paused' | 'error' | 'offline';
export function statusTone(st: PrinterStatus | undefined): StatusTone {
  if (!st?.online) return 'offline';
  if (st.state === 'failed' || st.state === 'error') return 'error';
  return st.state === 'paused' ? 'paused' : st.state === 'printing' || st.state === 'busy' ? 'printing' : 'online';
}
export function presenceStatus(live: boolean, connection: PrinterConnection, st: PrinterStatus | undefined, brief = false): { label: string; tone: StatusTone } {
  if (connection.kind === 'bambu-connect') return { label: 'Bambu Connect', tone: 'online' };
  if (!live) return { label: 'Reconnecting…', tone: 'offline' };
  if (!st?.online) return { label: st ? statusLine(st) : 'Connecting…', tone: 'offline' };
  if (st.state === 'printing' || st.state === 'paused')
    return { label: brief ? `${st.state === 'paused' ? 'Paused' : 'Printing'}${st.progress != null ? ` ${Math.round(st.progress * 100)}%` : ''}` : statusLine(st), tone: statusTone(st) };
  return { label: 'Online', tone: 'online' };
}
export function pickerStatus(c: PrinterConnections, printer: string): { label: string; tone: StatusTone } | undefined {
  const connection = connectionOf(c, printer);
  if (!connection) return undefined;
  if (connection.kind === 'bambu-connect') return { label: 'Bambu Connect', tone: 'online' };
  const st = c.status[printer];
  if (!st || !st.online) return { label: st ? statusLine(st) : 'Not connected', tone: 'offline' };
  return { label: statusLine(st), tone: statusTone(st) };
}
export const tempLine = (st: PrinterStatus | undefined, model = '') => [st?.nozzle && `Nozzle ${Math.round(st.nozzle.actual)}${st.nozzle.target ? `/${Math.round(st.nozzle.target)}` : ''} °C`,
  st?.bed && `Bed ${Math.round(st.bed.actual)}${st.bed.target ? `/${Math.round(st.bed.target)}` : ''} °C`,
  printerCapabilities(model).chamber && st?.chamber && `Chamber ${Math.round(st.chamber.actual)}${st.chamber.target ? `/${Math.round(st.chamber.target)}` : ''} °C`].filter(Boolean).join(SEP);
