import { urlProject } from './flow/routes';

export interface BootNote { tray: string; name: string; type: string }
export interface BootSlot { group?: boolean; report?: boolean; note?: BootNote }
export interface SlotOfferWire { strip: { filaments: string[]; colours: (string | null)[]; mixes: Record<string, unknown> }; placed: boolean[]; adds: string[]; differs: boolean; notes: string[];
  values: (Record<string, unknown> | null)[] | null; same: boolean }
export interface BootNotice { project: string; offer: SlotOfferWire }
export interface BootShape { network: boolean; project?: string; recovered?: 'interrupted' | 'conflict'; printer: string; connected: boolean; slots: BootSlot[]; railWidth?: number;
  picker?: { vendor: string; model: string; variant: string; nozzle?: number; nozzles?: number[] };
  projects?: number;
  folders?: string[]; notice?: BootNotice }

const strings = (v: unknown): v is string[] => Array.isArray(v) && v.every((x) => typeof x === 'string');
const offerValid = (o: SlotOfferWire | undefined) => !!o && !!o.strip && strings(o.strip.filaments) && Array.isArray(o.strip.colours) && typeof o.strip.mixes === 'object'
  && Array.isArray(o.placed) && strings(o.adds) && strings(o.notes) && typeof o.differs === 'boolean' && typeof o.same === 'boolean' && (o.values === null || Array.isArray(o.values));
const valid = (v: unknown): v is BootShape => {
  const b = v as BootShape;
  return !!b && typeof b.network === 'boolean' && (b.project === undefined || typeof b.project === 'string') && (b.recovered === undefined || b.recovered === 'interrupted' || b.recovered === 'conflict') && typeof b.printer === 'string' && typeof b.connected === 'boolean' && Array.isArray(b.slots) && b.slots.length <= 64
    && b.slots.every((s) => !!s && typeof s === 'object' && (s.note === undefined || (!!s.note && ['tray', 'name', 'type'].every((k) => typeof s.note![k as keyof BootNote] === 'string')))) && (b.railWidth === undefined || (typeof b.railWidth === 'number' && Number.isFinite(b.railWidth)))
    && (b.projects === undefined || (Number.isInteger(b.projects) && b.projects >= 0)) && (b.folders === undefined || strings(b.folders))
    && (b.picker === undefined || (!!b.picker && ['vendor', 'model', 'variant'].every((k) => typeof b.picker![k as 'vendor'] === 'string') && (b.picker.nozzle === undefined || typeof b.picker.nozzle === 'number') && (b.picker.nozzles === undefined || (Array.isArray(b.picker.nozzles) && b.picker.nozzles.length <= 16 && b.picker.nozzles.every((n) => typeof n === 'number')))))
    && (b.notice === undefined || (!!b.notice && typeof b.notice.project === 'string' && offerValid(b.notice.offer)));
};
const shape: BootShape | null = (() => {
  try { const v: unknown = JSON.parse(document.getElementById('pancake-boot')?.textContent ?? 'null'); return valid(v) ? v : null; } catch { return null; }
})();

export const bootPagePrinter = (): string | null => shape?.project && shape.project === urlProject() ? shape.printer : null;
export const bootShape = (printer?: string): BootShape | null => shape && (!printer || shape.printer === printer || !!bootPagePrinter()) ? shape : null;

export const statusLineShown = (enabled: boolean | null): boolean => enabled ?? shape?.network ?? true;

export const bootNotice = (project: string | null | undefined): BootNotice | null => project && shape?.notice?.project === project ? shape.notice : null;

export const bootRecovered = (): BootShape['recovered'] => shape?.project && shape.project === urlProject() ? shape.recovered : undefined;

export const bootProjects = (): number | undefined => shape?.projects;
export const bootFolders = (): string[] => shape?.folders ?? [];
