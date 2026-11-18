import { newProfileNameError } from './save-to-profile';
import { useEffect, useState } from 'react';
import type { ProcessRecord } from './records';
import { getState, type State } from '../state/store';
import { commitRecords } from './transactions';
import { randomId } from '../random-id';

import { responseError } from '../api/access';

export interface PrintMaster { id: string; name: string; description: string; layers: [number, number]; record: ProcessRecord }

let read: Promise<PrintMaster[]> | null = null;
export function printMasters(): Promise<PrintMaster[]> {
  read ??= fetch('/api/print-catalog').then(async (r) => { if (!r.ok) throw await responseError(r); return await r.json() as PrintMaster[]; })
    .catch((e) => { read = null; throw e; });
  return read;
}
export function usePrintMasters(enabled: boolean): { data?: PrintMaster[]; error?: string } {
  const [out, setOut] = useState<{ data?: PrintMaster[]; error?: string }>({});
  useEffect(() => {
    if (!enabled || out.data) return;
    let live = true;
    printMasters().then((data) => { if (live) setOut({ data }); }, (e) => { if (live) setOut({ error: e instanceof Error ? e.message : String(e) }); });
    return () => { live = false; };
  }, [enabled, out.data]);
  return out;
}

const round2 = (n: number) => Math.round(n * 100) / 100;
export function layerRange(s: State, m: PrintMaster): string {
  const nozzle = (s.records.printers.find((p) => p.id === s.printer)?.nozzles[0]?.diameter ?? 0) || 0.4;
  return `${round2(m.layers[0] * nozzle).toFixed(2)}–${round2(m.layers[1] * nozzle).toFixed(2)} mm`;
}

const printProfiles = (s: State) => s.records.processes;
export const isAdded = (s: State, m: PrintMaster) => printProfiles(s).some((p) => p.id === m.id && !p.archived);
export function freeName(s: State, name: string): string {
  const taken = new Set(printProfiles(s).map((p) => p.name));
  let out = name.trim();
  for (let n = 2; taken.has(out); n++) out = `${name.trim()} ${n}`;
  return out;
}

export async function addPrintMaster(m: PrintMaster): Promise<string> {
  const s = getState(), prior = printProfiles(s).find((p) => p.id === m.id);
  if (prior && !prior.archived) return m.id;
  const { archived: _a, ...restored } = prior ?? { ...m.record, name: freeName(s, m.name), revision: 0 };
  await commitRecords([{ kind: 'process', record: restored as ProcessRecord }], { label: 'Add print profile' });
  return m.id;
}

export async function createPrintProfile(base: { profile: ProcessRecord } | { master: PrintMaster }, name: string): Promise<string> {
  const s = getState(), wanted = name.trim(), error = newProfileNameError(s, 'process', wanted);
  if (error) throw new Error(error);
  const from = 'profile' in base ? base.profile : base.master.record, id = `profile:${randomId()}`;
  const { archived: _a, ...copy } = structuredClone(from);
  await commitRecords([{ kind: 'process', record: { ...copy, id, name: wanted, revision: 0 } as ProcessRecord }], { label: 'New print profile' });
  return id;
}
