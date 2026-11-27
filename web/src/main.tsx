import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import App, { preloadProjects } from './App';
import { urlRoute } from './flow/routes';
import { importFiles, init } from './actions';
import { startAgentBridge } from './agent-bridge';
import { project, startProjects } from './flow/project';
import { lease, leaseWritable } from './flow/project-lease';
import { versionView } from './flow/version-view';
import { loadTab } from './options';
import { initialiseSavedPreferences } from './saved-preferences';
import { preloadViews, whenIdle } from './preloadable';
import { preloadCovers, preloadImages, printerCatalog } from './profiles/catalog-reads';
import { printerCover } from './profiles/metadata';
import { listProfiles } from './profiles/pickerSource';
import { documentGeneration } from './state/history';
import { getState, resolvePreset, setState, subscribeStore } from './state/store';

function startLaunchImports() {
  const desktop = window.orcaDesktop;
  if (!desktop?.onLaunchFilesAvailable || !desktop.readLaunchFiles || !desktop.acceptLaunchFile) return;
  const { readLaunchFiles, acceptLaunchFile } = desktop;
  const destination = () => {
    const { value, context } = project.get();
    return context.loaded && !context.loadError && value !== 'failed' && value !== 'conflict'
      && versionView.get().value === 'latest' && leaseWritable() && getState().tab !== 'projects'
      ? documentGeneration() : null;
  };
  let requested = false;
  let running = false;
  async function drain() {
    try {
      while (requested) {
        requested = false;
        const generation = destination();
        if (generation === null) continue;
        const canImport = () => destination() === generation;
        let after = 0;
        while (canImport()) {
          const batch = await readLaunchFiles(after);
          if (!batch) break;
          after = batch.id;
          for (const file of batch.files) {
            if (!canImport()) break;
            const model = new File([file.data], file.name, { lastModified: file.lastModified });
            const imported = await importFiles([model], { canImport }).catch((error) => { console.error(error); return 0; });
            if (imported) await acceptLaunchFile(batch.id, file.index);
          }
        }
      }
    } finally {
      running = false;
      if (requested) request();
    }
  }
  const request = () => {
    requested = true;
    if (running) return;
    running = true;
    queueMicrotask(() => { void drain().catch(console.error); });
  };
  let previous = destination();
  const destinationChanged = () => {
    const current = destination();
    if (current !== null && current !== previous) request();
    previous = current;
  };
  project.subscribe(destinationChanged);
  lease.subscribe(destinationChanged);
  versionView.subscribe(destinationChanged);
  subscribeStore(destinationChanged);
  desktop.onLaunchFilesAvailable(request);
  request();
}

export function start() {
  void initialiseSavedPreferences().catch(() => {});

  void loadTab('process').catch(() => {});
  const booted = init();
  void startProjects(booted).then(() => { startLaunchImports(); startAgentBridge(); }).catch(() => {});
  void booted.finally(() => {
    preloadViews();
    whenIdle(() => preloadImages(listProfiles('machine').flatMap((e) => printerCover(e.vendor, {}, e.model) ?? [])));
    whenIdle(() => void printerCatalog().then((vendors) => {
      const s = getState();
      preloadCovers(vendors, ['BBL', resolvePreset(s, 'machine', s.printer).base?.vendor ?? 'BBL']);
    }).catch(() => {}));
  }).catch(() => {});

  const render = () => createRoot(document.getElementById('root')!).render(
    <StrictMode>
      <App />
    </StrictMode>,
  );
  if (urlRoute().kind === 'projects') { setState({ tab: 'projects' }); void preloadProjects().then(render, render); }
  else render();
  window.addEventListener('pagehide', () => { void import('./saved-preferences').then(m=>m.flushSavedPreferences()).catch(()=>{}); });
}
