import { openUrlImport } from '../url-import';
import { UrlImportInline } from './UrlImport';
import { preferenceText, savePreferences, savedPreferencesSnapshot, subscribeSavedPreferences, usePreference } from '../saved-preferences';
import { useEffect, useRef, useState, useSyncExternalStore } from 'react';
import { HoverCard } from 'radix-ui';
import { openFileDialog, openProjectDialog } from '../actions';
import { importFetched } from '../plater/handy';
import { getState, subscribeStore, toast, useStore } from '../state/store';
import { project } from '../flow/project';
import { currentRoute } from '../flow/routes';
import { recordOf } from '../profiles/view';
import { openSetup } from '../onboarding/setup-flow';
import { documentGeneration, onLeaveDocument } from '../state/history';
import './first-run.css';
import { slotFilaments } from '../profiles/slots';

const SETUP_KEY = 'orca.setup';
const assets = import.meta.glob<string>('../samples/*.{drc,webp}', { eager: true, query: '?url', import: 'default' });
type Variant = { slope?: number; file: string; model: string; preview: string };
type SampleDef = { label: string; variants: Variant[] };
const asset = (name: string) => assets[`../samples/${name}`];
const relief = (label: string, name: string, title: string, slopes: number[]): SampleDef => ({ label, variants: slopes.map((slope) => {
  const stem = `${name}${slope === 10 ? '' : `-${slope}`}`;
  return { slope, file: `${title} ${slope}°.drc`, model: asset(`${stem}.drc`), preview: asset(`${stem}.webp`) };
}) });
const SAMPLES: SampleDef[] = [
  { label: 'Benchy', variants: [{ file: '3DBenchy.drc', model: asset('benchy.drc'), preview: asset('benchy.webp') }] },
  relief('Waves', 'serpentine_waves', 'Serpentine waves', [10, 15, 27, 43, 47]),
  relief('Pyramids', 'four_pyramids', 'Four pyramids', [10, 15, 27, 43]),
  relief('Ridges', 'chevron_ridges', 'Chevron ridges', [10, 15, 27, 43]),
  relief('Ripples', 'ripple_cylinder', 'Ripple cylinder', [10, 15, 27, 43]),
  relief('Cylinder', 'wavy_cylinder', 'Wavy cylinder', [10, 15, 20, 27, 43]),
];
const models = new Map<string, Promise<string>>();
function modelBlobUrl(url: string) {
  let p = models.get(url);
  if (!p) {
    p = fetch(url).then(async (r) => {
      if (!r.ok) throw new Error(`${r.status} ${r.statusText}`);
      return URL.createObjectURL(await r.blob());
    });
    p.catch(() => models.delete(url));
    models.set(url, p);
  }
  return p;
}
const decoded = new Set<string>();
function warmPreview(url: string) {
  if (decoded.has(url)) return;
  decoded.add(url);
  const img = new Image(); img.src = url; void img.decode().catch(() => decoded.delete(url));
}
let warmed = false;
const emptyForGood = () => currentRoute().kind === 'blank' && project.get().value === 'blank' && !getState().objects.length && !getState().loading;
function warmSamples() {
  if (warmed) return;
  const idle = window.requestIdleCallback ?? ((f: () => void) => setTimeout(f, 200));
  idle(() => {
    if (warmed || !emptyForGood()) return;
    warmed = true;
    for (const s of SAMPLES) warmPreview(s.variants[0].preview);
    idle(() => { for (const s of SAMPLES) modelBlobUrl(s.variants[0].model).catch(() => {}); });
  });
}
function warmVariants(variants: Variant[]) {
  for (const v of variants.slice(1)) { warmPreview(v.preview); modelBlobUrl(v.model).catch(() => {}); }
}
const LONG_PRESS_MS = 450;
function Sample({ label, variants }: SampleDef) {
  const [open, setOpen] = useState(false), [busy, setBusy] = useState(false), [shown, setShown] = useState(0);
  const picked = useRef(false), chip = useRef<HTMLButtonElement>(null), card = useRef<HTMLDivElement>(null);
  const press = useRef<{ timer?: ReturnType<typeof setTimeout>; long: boolean }>({ long: false });
  const steps = variants.length > 1;
  const show = () => { setShown(0); warmVariants(variants); setOpen(true); };
  const close = () => { picked.current = true; setOpen(false); };
  const pick = async (v: Variant) => {
    close();
    if (busy) return;
    setBusy(true);
    try {
      await importFetched(label, [[await modelBlobUrl(v.model), v.file]]);
    } catch (e) {
      toast('couldnt_load_2', { label, detail: e instanceof Error ? e.message : String(e) });
    } finally {
      setBusy(false);
    }
  };
  const segments = () => [...(card.current?.querySelectorAll<HTMLButtonElement>('.seg > button') ?? [])];
  const endPress = () => clearTimeout(press.current.timer);
  return <HoverCard.Root open={open} openDelay={30} closeDelay={60}
    onOpenChange={(v) => { if (v) { if (!picked.current) show(); } else if (!card.current?.contains(document.activeElement)) setOpen(false); }}>
    <HoverCard.Trigger asChild>
      <button ref={chip} type="button" className="fr-sample" data-busy={busy || undefined} aria-busy={busy || undefined}
        aria-expanded={steps ? open : undefined}
        onPointerLeave={() => { picked.current = false; endPress(); }} onBlur={() => { picked.current = false; }}
        onPointerDown={(e) => { press.current.long = false; if (e.pointerType === 'touch') press.current.timer = setTimeout(() => { press.current.long = true; picked.current = false; show(); }, LONG_PRESS_MS); }}
        onPointerUp={endPress} onPointerCancel={endPress} onContextMenu={(e) => { if (press.current.long) e.preventDefault(); }}
        onKeyDown={(e) => { if (steps && (e.key === 'ArrowUp' || e.key === 'ArrowDown')) { e.preventDefault(); picked.current = false; show(); requestAnimationFrame(() => segments()[0]?.focus()); } }}
        onClick={() => { if (press.current.long) press.current.long = false; else void pick(variants[0]); }}>{label}</button>
    </HoverCard.Trigger>
    <HoverCard.Content ref={card} className="fr-preview" side="top" sideOffset={8} collisionPadding={8}
      onBlur={(e) => { const to = e.relatedTarget as Node | null; if (!e.currentTarget.contains(to) && to !== chip.current) setOpen(false); }}>
      <div className="sample-preview" aria-hidden style={{ backgroundImage: `url(${variants[shown].preview})` }} />
      {steps &&
        <div className="seg" role="group" aria-label={`${label} top slope`}
          onKeyDown={(e) => {
            const all = segments(), i = all.indexOf(document.activeElement as HTMLButtonElement);
            if (e.key === 'ArrowLeft' || e.key === 'ArrowRight') { e.preventDefault(); all[(i + (e.key === 'ArrowRight' ? 1 : all.length - 1)) % all.length]?.focus(); }
            else if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); close(); chip.current?.focus(); }
          }}>
          {variants.map((v, i) => <button key={v.file} type="button" tabIndex={i === shown ? 0 : -1} className={i === shown ? 'on' : undefined}
            aria-label={`Add ${label}, ${v.slope}° top slope`} onPointerEnter={() => setShown(i)} onFocus={() => setShown(i)}
            onClick={() => void pick(v)}>{v.slope}°</button>)}
        </div>}
    </HoverCard.Content>
  </HoverCard.Root>;
}


const MOD = /Mac|iPhone|iPad/.test(navigator.platform) ? '⌘' : 'Ctrl+';

function DropZone() {
  useEffect(warmSamples, []);
  return (
    <div className="fr-drop">
      <div className="fr-drop-title">Drop a model</div>
      <div className="fr-actions">
        <button type="button" className="btn-primary fr-add" onClick={openFileDialog}>
          Add model<kbd>{MOD}I</kbd>
        </button>
        <button type="button" className="btn-ghost" onClick={openProjectDialog}>Open project</button>
        <button type="button" className="btn-ghost" onClick={() => openUrlImport('', true)}>From URL</button>
      </div>
      <UrlImportInline />
      <div className="fr-samples">
        <span>or try</span>
        {SAMPLES.map((s) => <Sample key={s.label} {...s} />)}
      </div>
    </div>
  );
}

let firstVisitObserver: (() => void) | null = null;
onLeaveDocument(() => firstVisitObserver?.());
function afterFirstVisit() {
  if(firstVisitObserver || getState().objects.length)return;
  const generation=documentGeneration();
  const root = document.documentElement;
  let completed=false;
  const stop=()=>{unsubscribe();firstVisitObserver=null;delete root.dataset.nudge;};
  const check = () => {
    const s = getState(),preferences=savedPreferencesSnapshot();
    if(documentGeneration()!==generation || !preferences.hydrated || s.profileLibraryLoad.state!=='ready' || preferences.state==='failed' || s.job.state!=='idle'
      || !completed && (preferenceText(SETUP_KEY) || preferenceText('orca.printer'))) { stop(); return; }
    if(s.objects.length && !completed){completed=true;savePreferences({[SETUP_KEY]:'1'});root.dataset.nudge='slice';}
    else if(completed && !s.objects.length)stop();
  };
  const unsubscribe=subscribeStore(check);
  firstVisitObserver=stop;
}

const hasFilament = (s: ReturnType<typeof getState>) => { const r = slotFilaments(s)[0] ? recordOf(s, 'filament', slotFilaments(s)[0]) : undefined; return !!r && !r.archived; };

let setupOffered = false;
export default function EmptyPlate() {
  const preferences=useSyncExternalStore(subscribeSavedPreferences,savedPreferencesSnapshot);
  const setup=usePreference(SETUP_KEY),printer=usePreference('orca.printer');
  const library=useStore(s=>s.profileLibraryLoad.state);
  const filament=useStore(hasFilament);
  const firstVisit=preferences.hydrated && library==='ready' && preferences.state!=='failed' && !setup && (!printer || !filament);
  useEffect(()=>{if(firstVisit)afterFirstVisit();},[firstVisit]);
  useEffect(()=>{if(firstVisit && !setupOffered){setupOffered=true;openSetup();}},[firstVisit]);
  return (
    <div className="empty-plate">
      <div className="fr-card"><DropZone /></div>
    </div>
  );
}

export function SetupWizard({ close }: { close(): void }) {
  useEffect(() => { openSetup(); close(); }, []);
  return null;
}
