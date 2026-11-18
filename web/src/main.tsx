import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import App, { preloadProjects } from './App';
import { urlRoute } from './flow/routes';
import { init } from './actions';
import { startProjects } from './flow/project';
import { loadTab } from './options';
import { initialiseSavedPreferences } from './saved-preferences';
import { preloadViews, whenIdle } from './preloadable';
import { preloadCovers, preloadImages, printerCatalog } from './profiles/catalog-reads';
import { printerCover } from './profiles/metadata';
import { listProfiles } from './profiles/pickerSource';
import { getState, resolvePreset, setState } from './state/store';
export function start() {
  void initialiseSavedPreferences().catch(() => {});

  void loadTab('process').catch(() => {});
  const booted = init();
  void startProjects(booted).catch(() => {});
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
