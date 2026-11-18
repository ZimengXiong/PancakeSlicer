import { getState, setState, subscribeStore, toast } from '../state/store';
import type { Transfer } from '../flow/transfer';

export type SendTone = 'progress' | 'success' | 'waiting' | 'error';
export interface SendOutcome { tone: SendTone; title: string; detail: string }

const sentence = (s: string) => s.charAt(0).toUpperCase() + s.slice(1);
const mb = (n: number) => (n / 1e6).toFixed(1);

export function sendOutcome(t: Transfer): SendOutcome {
  if (t.state === 'uploading') return { tone: 'progress', title: `Sending to ${t.name}`, detail: `${mb(t.sent)} of ${mb(t.total)} MB (${Math.floor((100 * t.sent) / Math.max(1, t.total))}%)` };
  if (t.state === 'starting') return { tone: 'progress', title: `Sending to ${t.name}`, detail: 'Uploaded! Waiting for your printer to start the job…' };
  if (t.state === 'cancelled') return { tone: 'error', title: 'Upload canceled', detail: sentence(t.error ?? '') };
  switch (t.outcome) {
    case 'printing': return { tone: 'success', title: `Printing on ${t.name}`, detail: '' };
    case 'uploaded': return { tone: 'success', title: `Uploaded to ${t.name} (ready to start)`, detail: 'You can start the print right from your printer\'s screen.' };
    case 'unconfirmed': return { tone: 'waiting', title: `Not confirmed by ${t.name}`, detail: `The file is on ${t.name}, but it hasn't confirmed that the print started. Check its screen before sending again.` };
    case 'failed': return { tone: 'error', title: `Couldn't send to ${t.name}`, detail: sentence(t.error ?? '') };
    case undefined: throw new Error(`Upload ${t.id} finished without a status report from the printer.`);
  }
}

const OUTCOME_LABEL = { printing: 'Started', uploaded: 'Uploaded', unconfirmed: 'Not confirmed', failed: 'Send failed' } as const;
const END_LABEL = { finished: 'Finished', failed: 'Failed', cancelled: 'Cancelled' } as const;
export const outcomeLabel = (t: Transfer, running = false) => t.job_end ? END_LABEL[t.job_end] : running ? 'Printing'
  : t.state === 'cancelled' ? 'Cancelled' : t.outcome ? OUTCOME_LABEL[t.outcome] : 'Sending';
export const outcomeTone = (t: Transfer, running = false) => t.job_end === 'failed' || t.outcome === 'failed' ? 'error' : running && !t.job_end ? 'printing' : undefined;

import { openDevice } from '../navigation';
export { openDevice };

const SHORT_REASON: Record<string, string> = { credentials: 'check the access code', developer_mode: 'turn on Developer Mode', busy: 'the printer is busy',
  storage: 'check the SD card', timeout: 'the printer did not answer', refused: 'the printer declined it', connection: 'cannot reach the printer', interrupted: 'the service restarted' };

export function noticeSend(t: Transfer) {
  let last = '';
  const show = (x: Transfer) => {
    const key = `${x.state}:${x.outcome}`;
    if (key === last) return;
    last = key;
    if (x.state === 'done' && x.outcome === 'printing') toast('printing', { name: x.name }, { action: { label: 'View', run: () => openDevice(x.printer) } });
    else if (x.state === 'done' && x.outcome === 'uploaded') toast('uploaded', { name: x.name }, { action: { label: 'View', run: () => openDevice(x.printer) } });
    else if (x.state === 'done' && x.outcome === 'unconfirmed') toast('print_start_unconfirmed', { name: x.name }, { action: { label: 'View', run: () => openDevice(x.printer) } });
    else if (x.state === 'failed' || x.state === 'cancelled') (x.state === 'cancelled' ? toast('upload_canceled', undefined, { action: { label: 'Retry', run: () => setState({ appDialog: 'send' }) } }) : toast('couldnt_send_xx', { name: x.name, value: SHORT_REASON[x.reason ?? ''] ? `: ${SHORT_REASON[x.reason!]}` : '' }, { action: { label: 'Retry', run: () => setState({ appDialog: 'send' }) } }));
  };
  const settled = (x: Transfer) => x.state !== 'uploading' && x.state !== 'starting' && x.outcome !== 'unconfirmed';
  show(t);
  if (settled(t)) return;
  const stop = subscribeStore(() => {
    const x = getState().printerConnections.transfers.find((y) => y.id === t.id);
    if (!x) return;
    show(x);
    if (settled(x)) { stop(); clearTimeout(timer); }
  });
  const timer = setTimeout(stop, 10 * 60_000);
}
