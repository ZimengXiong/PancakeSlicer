import { getState, toast } from '../state/store';
import { projectBlob } from '../saveProject';
import { toolJob } from './meshOps';

let busy = false;

export async function exportModel(format: 'generic3mf' | 'drc') {
  const s = getState();
  if (busy || !s.api || s.api.mock) return;
  busy = true;
  (format === 'drc' ? toast('exporting_draco') : toast('exporting_generic_3mf'));
  try {
    const { record, blob } = await toolJob('export', { export_format: format }, [[await projectBlob(s), 'project.3mf']]);
    const base = (s.projectName ?? s.objects[0]?.name ?? s.projectPlates.flatMap((p) => p.objects)[0]?.name ?? 'model').replace(/\.(stl|obj|3mf|drc|step|stp)$/i, '');
    const href = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = href; a.download = `${base}.${format === 'drc' ? 'drc' : '3mf'}`; document.body.appendChild(a); a.click(); a.remove();
    setTimeout(() => URL.revokeObjectURL(href), 60000);
    const reasons = (record.fallback_reasons as string[] | undefined) ?? [];
    (record.best_effort ? toast('draco_export_best_effortx', { value: reasons.length ? ` (${reasons.join(', ')})` : '' }) : toast('export_finished'));
  } catch (e) {
    toast('export_failed', { detail: e instanceof Error ? e.message : String(e) });
  } finally {
    busy = false;
  }
}
