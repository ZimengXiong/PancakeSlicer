import type { State } from '../state/store';
import type { ProjectProfileReferences } from './ownership';
import type { FilamentRecord, PrinterRecord, ProcessRecord } from './records';
import { recordOf } from './view';
import { slotFilaments } from './slots';

const cache = new WeakMap<object, { inputs: unknown[]; value: ProjectProfileReferences }>();
export function captureProjectProfiles(s: Pick<State, 'records' | 'printer' | 'process'>): ProjectProfileReferences {
  const inputs = [s.records, s.printer, s.process], prior = cache.get(s.records);
  if (prior && prior.inputs.every((v, i) => v === inputs[i])) return prior.value;
  const printer = recordOf(s as State, 'machine', s.printer) as PrinterRecord | undefined;
  const value: ProjectProfileReferences = { schema: 2, printer, process: recordOf(s as State, 'process', s.process) as ProcessRecord | undefined,
    filaments: slotFilaments(s).map((key) => recordOf(s as State, 'filament', key) as FilamentRecord | undefined),
    ...(printer ? { slots: printer.slots } : {}) };
  cache.set(s.records, { inputs, value });
  return value;
}
