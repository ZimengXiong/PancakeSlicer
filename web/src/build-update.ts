import { projectDraftMutationSnapshot } from './persist/drafts';
import { getState, toast } from './state/store';

const CHECK_MS = 5 * 60_000, MIN_GAP_MS = 15_000;

const writers = new Set<() => boolean>();
export const holdReloadWhile = (busy: () => boolean) => { writers.add(busy); };

export function watchForNewBuild(): void {
  const loaded = document.querySelector<HTMLMetaElement>('meta[name="orca-build"]')?.content;
  if (!loaded) return;
  let last = 0, newer = false, held = false;
  const safe = () => {
    const drafts = projectDraftMutationSnapshot(), s = getState();
    return !held && drafts.pending === 0 && drafts.error === null && !['submitting', 'running'].includes(s.job.state)
      && ![...writers].some((busy) => busy()) && !Object.keys(s.libraryDrafts).length;
  };
  const apply = () => {
    if (!newer) return;
    if (safe()) location.reload();
    else toast('update_ready', undefined, { action: { label: 'Reload', run: () => location.reload() } });
  };
  const check = async (now = false) => {
    if (!now && (document.visibilityState !== 'visible' || Date.now() - last < MIN_GAP_MS)) return;
    last = Date.now();
    try {
      const current = (await fetch('/', { method: 'HEAD', cache: 'no-store' })).headers.get('X-Build');
      newer = !!current && current !== loaded;
    } catch { return; }
    apply();
  };
  window.addEventListener('pointerdown', () => { held = true; }, true);
  window.addEventListener('pointerup', () => { held = false; }, true);
  window.addEventListener('pointercancel', () => { held = false; }, true);
  window.addEventListener('vite:preloadError', () => void check(true).then(() => {
    let tries = 0;
    const retry = () => { if (newer && !safe() && ++tries <= 30) setTimeout(() => (safe() ? location.reload() : retry()), 1000); };
    retry();
  }));
  window.addEventListener('focus', () => void check());
  document.addEventListener('visibilitychange', () => void check());
  setInterval(() => void check(), CHECK_MS);
}
