import type { Config } from '../api/types';
import { createMachine } from './fsm';
import { getState, setState, subscribeStore, toast } from '../state/store';
import { onLeaveDocument, undoable } from '../state/history';
import { documentStrip, type Strip } from '../profiles/slots';
import { writeStrip } from '../profiles/slot-writes';
import { catalogList } from '../catalog/read';
import { addCatalogProducts } from '../profiles/catalog-add';
import { patchServiceSession, sessionDocument } from '../persist/service-session';
import { jsonWrite, serviceJson } from '../persist/service';
import { bootNotice, type SlotOfferWire } from '../boot-shape';
import { routing, urlProject } from './routes';
import { listed } from '../components/ProjectNotice';

export interface SlotOffer { strip: Strip; values: (Config | undefined)[]; notes: string[]; differs: boolean; same: boolean;
  placed: boolean[] }
const CATALOG = 'catalog:';
export const offerAddsText = (o: SlotOffer) => listed(offerAdds(o));
export const offerAdds = (o: SlotOffer) => [...new Set(o.strip.filaments.filter((f) => f.startsWith(CATALOG)).map((f) => f.slice(CATALOG.length)))];
type ChoiceState = 'none' | 'offered' | 'used' | 'kept' | 'dismissed' | 'failed';
type ChoiceEvent = { type: 'OFFER'; offer: SlotOffer } | { type: 'NONE' } | { type: 'FAIL'; error: string } | { type: 'USE' } | { type: 'KEEP' } | { type: 'DISMISS' } | { type: 'RESET' };
interface ChoiceContext { offer: SlotOffer | null; error?: string }

const offer = { target: 'offered', action: (_: ChoiceContext, e: { offer: SlotOffer }) => ({ offer: e.offer }) } as const;
const none = { target: 'none', action: () => ({ offer: null, error: undefined }) } as const;
const fail = { target: 'failed', action: (_: ChoiceContext, e: { error: string }) => ({ offer: null, error: e.error }) } as const;
const decided = { OFFER: offer, NONE: none, FAIL: fail, RESET: none, USE: {}, KEEP: {}, DISMISS: {} } as const;
const fromWire = (o: SlotOfferWire): SlotOffer =>
  ({ strip: { ids: o.strip.filaments.map(() => undefined), filaments: o.strip.filaments, colours: o.strip.colours.map((c) => c ?? undefined), mixes: o.strip.mixes as Strip['mixes'] },
    values: (o.values ?? []).map((v) => (v ?? undefined) as Config | undefined), notes: o.notes, differs: o.differs, same: o.same, placed: o.placed });
const answered = (wire: SlotOfferWire | null): SlotOffer => { if (!wire) throw new Error('the service sent no answer'); return fromWire(wire); };
const shown = (o: SlotOffer) => o.differs || o.same || o.notes.length > 0;
const served = bootNotice(urlProject());
export const projectSlots = createMachine<ChoiceState, ChoiceEvent, ChoiceContext>({
  id: 'project-slots', initial: served ? 'offered' : 'none', strict: true, context: { offer: served ? fromWire(served.offer) : null },
  states: {
    none: { on: decided },
    offered: { on: { ...decided, KEEP: 'kept', DISMISS: 'dismissed', USE: { target: 'used', action: (c) => { void use(c.offer!); } } } },
    used: { on: { ...decided, DISMISS: 'dismissed' } },
    kept: { on: decided },
    dismissed: { on: decided },
    failed: { on: { ...decided, DISMISS: 'dismissed' } },
  },
});

export const choicePending = (value: ChoiceState, o: SlotOffer | null) => value === 'offered' && !!o && (o.differs || o.same);
export const offerUndecided = () => { const { value, context } = projectSlots.get(); return choicePending(value, context.offer); };
export const OFFER_UNDECIDED = 'Choose Use project\'s or Keep printer\'s for this project\'s filaments first.';

export function noticeOf(value: ChoiceState, o: SlotOffer | null, error?: string): { choice: boolean; adds: string; notes: string[]; same: boolean } | null {
  if (value === 'failed') return { choice: false, adds: '', notes: [`Couldn't compare this project's filaments with the printer's slots: ${error ?? 'the service did not answer'}`], same: false };
  const choice = choicePending(value, o);
  const notes = o && (value === 'offered' || value === 'used') ? o.notes : [];
  return choice || notes.length ? { choice, adds: choice ? offerAddsText(o!) : '', notes, same: choice && o!.same } : null;
}

const openProject = () => { const r = routing.get(); return r.value === 'project' ? r.context.id : urlProject(); };
type StoredChoice = { at: number; dismissed?: true };
const dismissed = (id: string) => !!(sessionDocument().projectSlotNotices as Record<string, StoredChoice> | undefined)?.[id]?.dismissed;
function ownStrip(strip: Strip | null) {
  const next = strip && { filaments: strip.filaments, colours: strip.colours, mixes: strip.mixes as Record<number, unknown> };
  if (JSON.stringify(getState().projectSlots) !== JSON.stringify(next)) setState({ projectSlots: next });
}
projectSlots.subscribe(({ value }) => { if (value !== 'offered') ownStrip(null); });
projectSlots.subscribe(({ value }) => {
  const id = openProject();
  if (!id || (value !== 'kept' && value !== 'dismissed') || dismissed(id)) return;
  patchServiceSession({ projectSlotNotices: { [id]: { dismissed: true, at: Date.now() } } });
});

async function use(o: SlotOffer) {
  const { strip: planned, values } = o, products = offerAdds(o);
  const added = new Map<string, string>();
  if (products.length) {
    try {
      const entries = (await catalogList('filaments')).entries;
      await addCatalogProducts(products.flatMap((id) => entries.filter((e) => e.id === id).map((product) => ({ product }))));
      for (const id of products) { const r = getState().records.filaments.find((f) => f.product === id && !f.archived); if (r) added.set(id, r.id); }
    } catch (e) { toast('couldnt_add_projects_filaments', { detail: e instanceof Error ? e.message : String(e) }); return; }
  }
  const s = getState(), filaments = planned.filaments.map((f) => f.startsWith(CATALOG) ? added.get(f.slice(CATALOG.length)) ?? '' : f);
  const { placeProject, followPlacement, printerName } = await import('./project-placement');
  const placed = placeProject(s, planned, filaments, o.placed);
  undoable("Use project's filaments", () => {
    if (!writeStrip(placed.strip, "Project's filaments")) return;
    setState(followPlacement(getState(), placed.map));
    const edits = values.flatMap((v, j) => v && Object.keys(v).length && filaments[j] && placed.map[j] <= placed.strip.filaments.length ? [[filaments[j], v] as const] : []);
    if (edits.length) setState((x) => ({ filamentEdits: { ...x.filamentEdits, ...Object.fromEntries(edits.map(([id, v]) => [id, { ...x.filamentEdits[id], ...v }])) } }));
  });
  const adds = [...added.keys()];
  if (adds.length) toast('added_library', { value: listed(adds) });
  if (placed.unplaced.length) toast('project_filaments_not_placed', { value: listed(placed.unplaced), printer: printerName(s), replaced: placed.replaced.join('\n') });
  else if (placed.replaced.length) toast('project_slots_replaced', { lines: placed.replaced.join('\n') });
}

let waiting: (() => void) | null = null;
let servedFor = served?.project ?? null;

export async function documentOffer(doc: { printer?: string; filamentEdits?: Record<string, Config>; projectSlotValues?: (Config | null)[] | null } & Parameters<typeof documentStrip>[0]): Promise<SlotOffer | Error | null> {
  const strip = documentStrip(doc);
  if (!strip) return null;
  return serviceJson<SlotOfferWire>('/api/slot-offer', jsonWrite('POST', { printer: doc.printer || getState().printer, strip,
    values: doc.projectSlotValues ?? [], edits: doc.filamentEdits ?? {} })).then(answered).catch((e: unknown) => e instanceof Error ? e : new Error(String(e)));
}
export function settleOffer(o: SlotOffer | Error | null): void {
  waiting?.(); waiting = null;
  const id = openProject();
  if (id && id === servedFor) { servedFor = null; if (projectSlots.get().value !== 'offered') ownStrip(null); return; }
  servedFor = null;
  if (id && dismissed(id)) projectSlots.send({ type: 'DISMISS' });
  else if (o instanceof Error) projectSlots.send({ type: 'FAIL', error: o.message });
  else if (o && shown(o)) projectSlots.send({ type: 'OFFER', offer: o });
  else projectSlots.send({ type: 'NONE' });
}
export function offerStrip(strip: Strip | null, values: (Config | undefined)[] = []): void {
  waiting?.(); waiting = null;
  const id = openProject();
  if (id && id === servedFor) { servedFor = null; return; }
  servedFor = null;
  let cancelled = false;
  const decide = async () => {
    if (!strip) { projectSlots.send({ type: 'NONE' }); return; }
    const s = getState();
    let plan: SlotOffer;
    try { plan = answered(await serviceJson<SlotOfferWire>('/api/slot-offer', jsonWrite('POST', { printer: s.printer, strip, values: values.map((v) => v ?? null), edits: s.filamentEdits }))); }
    catch (e) { if (!cancelled) projectSlots.send({ type: 'FAIL', error: e instanceof Error ? e.message : String(e) }); return; }
    if (cancelled) return;
    if (id && dismissed(id)) projectSlots.send({ type: 'DISMISS' });
    else if (shown(plan)) { ownStrip(strip); projectSlots.send({ type: 'OFFER', offer: plan }); }
    else projectSlots.send({ type: 'NONE' });
  };
  waiting = () => { cancelled = true; };
  if (getState().profileLibraryLoad.state === 'ready') { void decide(); return; }
  const stop = subscribeStore(() => { if (getState().profileLibraryLoad.state !== 'ready') return; stop(); void decide(); });
  waiting = () => { cancelled = true; stop(); };
}
onLeaveDocument(() => {
  if (servedFor && servedFor === urlProject()) return;
  servedFor = null; waiting?.(); waiting = null; projectSlots.send({ type: 'RESET' });
});
