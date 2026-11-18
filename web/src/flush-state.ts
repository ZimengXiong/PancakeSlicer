import type { State } from './state/store';
import { plateSetup } from './profiles/plate-setup';
import { slotFilaments } from './profiles/slots';

export const MULTIPLIER_RANGE: [number, number] = [0, 3];
export const MAX_FLUSH_VOLUME = 20000;

export function nozzleFlush(s: State, nozzle = 0) {
  return plateSetup(s)?.flushing?.nozzles[nozzle] ?? null;
}

export function allFlush(s: State): { matrix: number[]; multipliers: number[] } | null {
  const nozzles = plateSetup(s)?.flushing?.nozzles;
  return nozzles ? { matrix: nozzles.flatMap((n) => n.matrix), multipliers: nozzles.map((n) => n.multiplier) } : null;
}

export function flushUnavailable(s: State): string {
  if (slotFilaments(s).filter(Boolean).length < 2) return 'Pop filaments into at least two slots to see flushing volumes!';
  if (allFlush(s)) return '';
  const r = s.resolved;
  if (!s.printer || !s.process) return 'Pick your print settings to check out the flushing volumes.';
  if (r.state === 'failed') return `Oops! Couldn't fetch the flushing volumes: ${r.error ?? 'the slicing service didn\'t answer'}.`;
  if (r.state === 'resolving' || r.answeredFor?.printer !== s.printer) return 'Hanging tight for the slicing service…';
  return 'Hmm, the slicing service didn\'t return any flushing volumes for this printer\'s filaments.';
}

export function flushModified(s: State): boolean {
  const flushing = s.records.printers.find((p) => p.id === s.printer)?.flushing;
  return !!plateSetup(s)?.flushing && !!(flushing?.volumes?.length || flushing?.multipliers?.length);
}
