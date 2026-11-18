import type { Config } from '../api/types';
import type { FlushingRecord } from '../profiles/records';
import type { State } from './store';

type Stored = { dirty?: State['dirty']; bedType?: string; flush?: { matrix?: number[] | null; multipliers?: number[] | null; multiplier?: number | null } };
export interface StoredPlateSetup { plate: string; flushing: FlushingRecord | null }

const numbers = (v: Config[string] | undefined): number[] | null => {
  const list = (Array.isArray(v) ? v : v === undefined ? [] : String(v).split(',')).map(Number);
  return list.length && list.every(Number.isFinite) ? list : null;
};

export function movePlateSetup<T extends Stored>(doc: T): Omit<T, 'bedType' | 'flush'> & { dirty: State['dirty']; plateSetup: StoredPlateSetup } {
  const { bedType, flush, ...rest } = doc;
  const dirty = { machine: { ...doc.dirty?.machine }, process: { ...doc.dirty?.process } };
  let plate = bedType ?? '';
  for (const edits of [dirty.machine, dirty.process]) {
    if (edits.curr_bed_type !== undefined && !plate) plate = String(edits.curr_bed_type);
    delete edits.curr_bed_type;
  }
  const volumes = flush?.matrix ?? numbers(dirty.process.flush_volumes_matrix);
  const multipliers = flush?.multipliers ?? numbers(dirty.process.flush_multiplier) ?? (flush?.multiplier != null ? [flush.multiplier] : null);
  delete dirty.process.flush_multiplier; delete dirty.process.flush_volumes_matrix;
  const flushing = volumes || multipliers ? { ...(volumes ? { volumes } : {}), ...(multipliers ? { multipliers } : {}) } : null;
  return { ...rest, dirty, plateSetup: { plate, flushing } };
}
