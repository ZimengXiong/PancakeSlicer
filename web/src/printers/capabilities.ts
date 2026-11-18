import type { State } from '../state/store';
import { recordOf } from '../profiles/view';
import type { PrinterRecord } from '../profiles/records';

export const printerModel = (state: State, printer: string): string => (recordOf(state, 'machine', printer) as PrinterRecord | undefined)?.model ?? '';

export function printerCapabilities(model: string) {
  const openBed = /\bA1(?:\s+mini)?\b/i.test(model);
  return { chamber: !openBed, timelapse: !/\bP1P\b/i.test(model), fans: openBed ? ['part'] : ['part', 'auxiliary', 'chamber'],
    lightLabel: openBed ? 'Light' : 'Chamber light', cameraLabel: openBed ? 'Camera' : undefined };
}
