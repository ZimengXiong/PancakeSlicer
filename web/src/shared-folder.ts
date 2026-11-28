import { canExport, exportBlocked, exportFilename, importFiles, openProject, outputKind } from './actions';
import { mutation, responseError, type ApiError } from './api/access';
import { browseShare, type SharePick } from './components/SharedFolder';
import { confirmDialog } from './components/Ask';
import { output } from './flow/output';
import { projectBlob, projectFileName } from './saveProject';
import { markSaved, snapshot } from './state/history';
import { getState, setState, toast } from './state/store';

const MODELS = ['.stl', '.obj', '.3mf', '.step', '.stp'];

async function readShare(pick: SharePick): Promise<File[]> {
  return Promise.all(pick.paths.map(async (path) => {
    const r = await fetch(`/api/share/file?path=${encodeURIComponent(path)}`);
    if (!r.ok) throw await responseError(r);
    return new File([await r.blob()], path.split('/').pop()!);
  }));
}

export async function openFromShare(kind: 'project' | 'models') {
  const pick = kind === 'project'
    ? await browseShare({ title: 'Open project from shared folder', action: 'Open', extensions: ['.3mf'] })
    : await browseShare({ title: 'Import from shared folder', action: 'Import', extensions: MODELS, multiple: true });
  if (!pick) return;
  let files: File[];
  try {
    files = await readShare(pick);
  } catch (e) {
    return void toast('couldnt_load', { name: pick.name, detail: e instanceof Error ? e.message : String(e) });
  }
  if (kind === 'project') await openProject(files[0]);
  else await importFiles(files);
}

const replace = (name: string) => confirmDialog('Replace file?', `${name} is already in the shared folder. Replace it?`, ['Replace', 'Cancel'], true);

async function writeShare(pick: SharePick, body: Blob, overwrite: boolean): Promise<boolean> {
  const path = `/api/share/file?path=${encodeURIComponent(pick.paths[0])}`;
  try {
    await mutation(overwrite ? `${path}&overwrite=1` : path, { method: 'PUT', body }, false, undefined, undefined, true);
    return true;
  } catch (e) {
    if ((e as ApiError).code !== 'share_file_exists' || overwrite || !await replace(pick.name)) throw e;
    return writeShare(pick, body, true);
  }
}

async function pickTarget(title: string, name: string, extension: string): Promise<SharePick | null> {
  const pick = await browseShare({ title, action: 'Save', extensions: [extension], save: name });
  return pick && (!pick.exists || await replace(pick.name)) ? pick : null;
}

export async function saveProjectToShare() {
  const s = getState();
  if (!s.api || !s.objects.length && !s.projectPlates.some((p) => p.objects.length)) return;
  if (s.api.mock) return void toast('saving_projects_requires_connection');
  const first = s.objects[0] ?? s.projectPlates.flatMap((p) => p.objects)[0];
  const pick = await pickTarget('Save project to shared folder', projectFileName(s.projectName ?? first.name.replace(/\.(stl|obj|3mf)$/i, '')), '.3mf');
  if (!pick) return;
  const task = output.begin('preparing');
  try {
    const blob = await projectBlob(s, task);
    if (!task.current()) return;
    task.progress(0, 'uploading');
    await writeShare(pick, blob, pick.exists);
    setState({ projectName: pick.name });
    markSaved(snapshot(s));
    task.done();
    toast('saved_shared_folder', { name: pick.name });
  } catch (e) {
    if (!task.current()) return;
    task.fail(e);
    toast('couldnt_save_refused', { detail: e instanceof Error ? e.message : e });
  }
}

export async function exportToShare() {
  const s = getState();
  if (!s.api || !canExport(s) || !s.job.id) return void (s.api && exportBlocked(s));
  const pick = await pickTarget('Export to shared folder', exportFilename(s), `.${outputKind(s)}`);
  if (!pick) return;
  const task = output.begin('exporting');
  try {
    const r = await fetch(s.api.downloadUrl(s.job.id), { signal: task.signal });
    if (!r.ok) throw await responseError(r);
    await writeShare(pick, await r.blob(), pick.exists);
    setState((st) => ({ job: { ...st.job, exported: pick.name } }));
    task.done();
    toast('saved_shared_folder', { name: pick.name });
  } catch (e) {
    if (!task.current()) return;
    task.fail(e);
    toast('export_failed', { detail: e instanceof Error ? e.message : e });
  }
}
