import { download, importFiles, openFileDialog, openProjectDialog, pickModelFiles, saveProject } from '../actions';
import { registerCommand } from '../commands/registry';
import { whenFresh } from '../flow/slice';
import { getState, setState } from '../state/store';

const api = () => import('./api');

export const fileVerbs = {
  new: () => void api().then((m) => m.newProject()),
  open: openProjectDialog,
  projects: () => setState({ tab: 'projects' }),
  import: () => { if (getState().tab === 'projects') pickModelFiles((files) => newProjectFrom(files)); else openFileDialog(); },
  'export-gcode': () => whenFresh(download),
  'export-3mf': () => void saveProject(),
  undo: () => void import('../state/history').then((m) => m.undo()),
  redo: () => void import('../state/history').then((m) => m.redo()),
} as const;
export type FileVerb = keyof typeof fileVerbs;

export async function newProjectFrom(files: FileList | File[]) {
  if(await (await api()).newProject())await importFiles(files);
}

window.addEventListener('orca-desktop-menu', (e) => {
  const verb = (e as CustomEvent<string>).detail;
  if (verb in fileVerbs) fileVerbs[verb as FileVerb]();
});

registerCommand([
  { id: 'file.projects', title: 'Projects', group: 'File', keywords: 'open recent home library', run: fileVerbs.projects, boost: 40 },
  { id: 'file.new', title: 'New project', group: 'File', keys: 'Mod+N', keywords: 'clear empty blank', run: fileVerbs.new },
  { id: 'file.exportproject', title: 'Save project', group: 'File', keys: 'Mod+S', keywords: '3mf save copy file', enabled: () => getState().objects.length > 0, run: fileVerbs['export-3mf'] },
]);
