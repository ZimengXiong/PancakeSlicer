import { jobRequest } from './actions';
import { getState, setState, toast, type State } from './state/store';
import { promptDialog } from './components/Ask';
import { markSaved, snapshot } from './state/history';
import { output } from './flow/output';
import type { OperationTicket } from './flow/operation';
import { waitForJob } from './flow/job';


export async function projectBlob(s: State, task?: OperationTicket): Promise<Blob> {
  const request = await jobRequest(s, true);
  task?.signal.throwIfAborted();
  task?.progress(0, 'uploading');
  const id = await s.api!.submit({ ...request, plate_index: undefined, output: '3mf' }, task?.signal,
    (fraction) => task?.progress(100 * fraction, 'uploading'));
  task?.progress(0, 'saving');
  await waitForJob(s.api!, id, task?.signal, (p) => task?.progress(p.pct, 'saving'));
  task?.signal.throwIfAborted();
  task?.progress(0, 'downloading');
  const response = await fetch(s.api!.downloadUrl(id), { signal: task?.signal });
  if (!response.ok) throw new Error(`${response.status} ${response.statusText}`);
  return response.blob();
}

export function projectFileName(name: string): string {
  // eslint-disable-next-line no-control-regex
  const base = name.replace(/\.3mf$/i, '').replace(/[<>:/\\|?*"\u0000-\u001f]/g, '_').replace(/[. ]+$/, '').trim();
  return `${base || 'project'}.3mf`;
}

export async function saveProject(as = false) {
  const s = getState();
  if (!s.api || !s.objects.length && !s.projectPlates.some((p) => p.objects.length)) return;
  if (s.api.mock) { toast('saving_projects_requires_connection'); return; }
  const first = s.objects[0] ?? s.projectPlates.flatMap((p) => p.objects)[0];
  let name = s.projectName ?? (first.name.replace(/\.(stl|obj|3mf)$/i, '') + '.3mf');
  if (as) {
    const proposed = await promptDialog('Save Project as', name, (v) => !v.replace(/\.3mf$/i, '').trim() ? 'Enter a name.'
      : new TextEncoder().encode(v.replace(/\.3mf$/i, '') + '.3mf').length > 240 ? 'Whoa, that name\'s a bit too long for a file — trim it down a little!' : null);
    if (!proposed) return;
    name = proposed;
  }
  name = projectFileName(name);
  const task = output.begin('preparing');
  try {
    const blob = await projectBlob(s, task);
    if (!task.current()) return;
    const href = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = href; a.download = name; document.body.appendChild(a); a.click(); a.remove();
    setTimeout(() => URL.revokeObjectURL(href), 60000);
    setState({ projectName: name });
    markSaved(snapshot(s));
    task.done();
    toast('downloading_project');
  } catch (e) {
    if (!task.current()) return;
    task.fail(e);
    const { code, status } = (e ?? {}) as { code?: string; status?: number };
    toast(code || status && status < 500 ? 'couldnt_save_refused' : 'couldnt_save', { detail: e instanceof Error ? e.message : e });
  }
}
