import { beginBusy } from '../state/busy';
import { arrangeAll, importFiles } from '../actions';
import { toast } from '../state/store';
import { documentGeneration } from '../state/history';

export const HANDY: [label: string, files: string[], arrange?: boolean][] = [
  ['Orca Cube', ['OrcaCube_v2.drc', 'OrcaPlug_v2.drc'], true],
  ['OrcaSliced Combo', ['OrcaSliced.3mf', 'OrcaCube_v2.drc', 'OrcaPlug_v2.drc'], true],
  ['Orca Badge', ['OrcaBadge.3mf']],
  ['Orca Tolerance Test', ['OrcaToleranceTest.drc']],
  ['3DBenchy', ['3DBenchy.drc']],
  ['Cali Cat', ['calicat.drc']],
  ['Autodesk FDM Test', ['ksr_fdmtest_v4.drc']],
  ['Voron Cube', ['Voron_Design_Cube_v7.drc']],
  ['Stanford Bunny', ['Stanford_Bunny.drc']],
  ['Orca String Hell', ['Orca_stringhell.drc']],
];

export async function importFetched(label: string, sources: [url: string, name: string][], arrange = false) {
  const generation = documentGeneration(), busy = beginBusy(`Loading ${label}`);
  try {
    const files = await Promise.all(sources.map(async ([url, name]) => {
      const r = await fetch(url);
      if (!r.ok) throw new Error(`${label}: ${r.status} ${r.statusText}`);
      return new File([await r.blob()], import.meta.env.MODE === 'demo' ? name.replace(/\.(drc|3mf)$/i, '.stl') : name);
    }));
    if (generation !== documentGeneration()) return;
    await importFiles(files);
    if (arrange && generation === documentGeneration()) arrangeAll(false);
  } catch (e) {
    toast('couldnt_load_2', { label, detail: e instanceof Error ? e.message : String(e) });
  } finally {
    busy.end();
  }
}

export const handyUrl = (name: string) => `/api/handy-models/${encodeURIComponent(name)}`;

export function addHandyModel(i: number) {
  const [label, names, arrange] = HANDY[i];
  return importFetched(label, names.map((name) => [handyUrl(name), name]), arrange);
}
