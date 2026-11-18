import { getState } from '../state/store';

const KEY = 'pancake.rail-layout.v1';
export interface RailRow { top: number; height: number; head?: string; group?: 'slots' | 'printer' }
export interface RailSectionBox { height: number; rows: RailRow[] }
export interface RailLayout { printer: string; slots: number; width: number; railWidth: number | null; sections: RailSectionBox[] }

const finite = (n: unknown, max: number): n is number => typeof n === 'number' && Number.isFinite(n) && n >= 0 && n <= max;
function valid(v: unknown): v is RailLayout {
  const l = v as RailLayout;
  return !!l && typeof l.printer === 'string' && finite(l.slots, 64) && finite(l.width, 4000) && (l.railWidth === null || finite(l.railWidth, 4000)) && Array.isArray(l.sections) && l.sections.length <= 4
    && l.sections.every((s) => finite(s.height, 20000) && Array.isArray(s.rows) && s.rows.length <= 64
      && s.rows.every((r) => finite(r.top, 20000) && finite(r.height, 4000) && (r.head === undefined || (typeof r.head === 'string' && r.head.length <= 40)) && (r.group === undefined || r.group === 'slots' || r.group === 'printer')));
}

const read = (): RailLayout | null => {
  try { const raw = localStorage.getItem(KEY), l: unknown = raw && JSON.parse(raw); return valid(l) ? l : null; } catch { return null; }
};
export const lastRailWidth = (): number | null | undefined => read()?.railWidth;

export function railLayout(printer: string, width: number, slots?: number): RailLayout | null {
  const l = read();
  return l && Math.abs(l.width - width) < 1 && (!printer || l.printer === printer) && (slots === undefined || l.slots === slots) ? l : null;
}

export function railSlotsHeight(printer: string, width: number, slots: number): number | null {
  const l = printer ? railLayout(printer, width, slots) : null;
  return l?.sections.flatMap((s) => s.rows).find((r) => r.group === 'slots')?.height ?? null;
}

export function rememberRailLayout(aside: HTMLElement, railWidth: number | null) {
  const s = getState();
  if (!s.printer || aside.querySelector('.rail-slots[data-kept]')) return;
  const sections = [...aside.querySelectorAll<HTMLElement>(':scope > section.rail-section')].map((section): RailSectionBox => {
    const box = section.getBoundingClientRect(), inner = box.top + section.clientTop;
    return { height: box.height, rows: [...section.children].map((child): RailRow => {
      const r = child.getBoundingClientRect(), head = child.matches('header.rail-head') ? child.querySelector('.rail-title-text')?.textContent ?? '' : undefined;
      return { top: r.top - inner, height: r.height, ...(head !== undefined ? { head } : {}), ...(child.matches('.rail-slots') ? { group: 'slots' as const } : child.matches('.rail-printer') ? { group: 'printer' as const } : {}) };
    }) };
  });
  const record = s.records.printers.find((p) => p.id === s.printer);
  const layout: RailLayout = { printer: s.printer, slots: record?.slots.length ?? 0, width: aside.getBoundingClientRect().width, railWidth, sections };
  if (!valid(layout)) return;
  try { localStorage.setItem(KEY, JSON.stringify(layout)); } catch {  }
}
