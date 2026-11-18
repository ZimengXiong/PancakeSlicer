import { beginBusy, startupBusy } from '../state/busy';
import { app } from '../flow/app';
import { importing } from '../flow/import';
import { loadTab } from '../options';
import { getState, setState } from '../state/store';
import { documentGeneration } from '../state/history';
import { initialiseSavedPreferences, preferenceText } from '../saved-preferences';
import { slotFilaments } from './slots';
let catalogLoadGeneration = 0;
const transient = (error: unknown) => error instanceof TypeError || [408, 429].includes((error as {status?:number}).status ?? 0) || ((error as {status?:number}).status ?? 0) >= 500;
let retryDelay = 1000, armed: (() => void) | null = null;
function disarmRetry() { armed?.(); armed = null; }
function retryCatalog(generation: number) {
  disarmRetry();
  const again = () => { disarmRetry(); if (generation === catalogLoadGeneration) void loadCatalog(); };
  const timer = setTimeout(again, retryDelay); window.addEventListener('online', again);
  armed = () => { clearTimeout(timer); window.removeEventListener('online', again); };
  retryDelay = Math.min(retryDelay * 2, 10_000);
}
let catalogLibraryTail: Promise<void> = Promise.resolve();
export async function loadCatalog() {
  disarmRetry();
  const generation = ++catalogLoadGeneration, initial = getState(), api = initial.api, documentAtStart=documentGeneration();
  if (!api) return;
  const busy = beginBusy('Loading presets');
  startupBusy.end();
  setState({ profileLibraryLoad: {state: 'loading'} });
  try {
  const current = () => generation === catalogLoadGeneration && getState().api === api;
  const settings = loadTab('process');
  const library = catalogLibraryTail.then(async () => {
    try {
      const [storage,{prepareProfileLibraryHydration},{watchPrinterSetups},{libraryWritesIdle,knowLibrary}]=await Promise.all([
        import('../persist/profile-library'),import('./library-hydration'),import('./printer-setups'),import('./library-writes'),import('./transactions')]);
      const {libraryWrites}=await import('./library-writes');
      await libraryWritesIdle();
      if(!current())return;
      const [records]=await Promise.all([storage.readLibrary(),settings]);
      if(!current())return;
      const unchanged=()=>{const s=getState();return current() && documentGeneration()===documentAtStart && s.project===initial.project && s.profileSnapshots===initial.profileSnapshots && s.projectPresets===initial.projectPresets && s.dirty===initial.dirty && s.records===initial.records && s.printer===initial.printer && s.process===initial.process && s.filamentEdits===initial.filamentEdits;};
      const restoreStartupPlaceholder=!initial.printer && !initial.process && !slotFilaments(initial).length && !initial.project && !initial.objects.length && !initial.profileSnapshots && !initial.projectPresets && !Object.keys(initial.dirty.machine).length && !Object.keys(initial.dirty.process).length && !Object.keys(initial.filamentEdits).length;
      if(!current())return;
      if(!unchanged())throw new Error('Your setup changed while profiles were loading. We kept your newest changes—just reload profiles to sync up.');
      if(restoreStartupPlaceholder){
        busy.end();
        await initialiseSavedPreferences();
      }
      if(!unchanged())throw new Error('Your setup changed while preferences were loading. Click Retry so you don\'t lose your latest changes.');
      const latest=getState(),preferred=restoreStartupPlaceholder ? preferenceText('orca.printer') : undefined;
      const patch=prepareProfileLibraryHydration(latest,records,{startup:restoreStartupPlaceholder,remembered:preferred});
      setState({...patch,profileLibraryLoad:{state:'ready'}});
      libraryWrites.send({type:'LIBRARY_READY'});
      knowLibrary(records);watchPrinterSetups();retryDelay=1000;
      void import('./library-events').then(m=>m.watchLibraryRevisions());
      void import('../flow/resolved').then(m=>m.watchResolved());
      void import('./view').then(m=>m.watchCatalogLines());
    } catch (error) {
      if(current()){
        setState({profileLibraryLoad:{state:'failed',error:error instanceof Error?error.message:String(error)}});
        if(transient(error))retryCatalog(generation);
      }
    }
  });
  catalogLibraryTail=library;
  await library;
  if (!current()) return;
  busy.end();
  app.send({type: 'READY', hasObjects: getState().objects.length > 0, importing: importing.machine.get().value === 'running'});
  } finally { busy.end(); }
}
