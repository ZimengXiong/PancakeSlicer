import { useSyncExternalStore } from 'react';
import { createMachine, type StateNode } from './fsm';
import { getState, subscribeStore, type MainTab } from '../state/store';

export type Page = MainTab;
type PageEvent = { type: 'SHOW'; page: Page };
interface PageContext { previous: Page | null }

const leaveHooks = new Map<Page, Set<() => void>>();
export function onLeavePage(page: Page, fn: () => void): () => void {
  let hooks = leaveHooks.get(page);
  if (!hooks) leaveHooks.set(page, (hooks = new Set()));
  hooks.add(fn);
  return () => { hooks!.delete(fn); };
}
const leave = (page: Page) => () => { for (const fn of [...leaveHooks.get(page) ?? []]) fn(); };

const PAGES: Page[] = ['projects', 'prepare', 'preview', 'device', 'library'];
const states = {} as Record<Page, StateNode<Page, PageEvent, PageContext>>;
for (const p of PAGES) states[p] = {
  exit: leave(p),
  on: { SHOW: [{ guard: (_, e) => e.page === p }, ...PAGES.map((t) => ({ target: t, guard: (_: PageContext, e: PageEvent) => e.page === t, action: () => ({ previous: p }) }))] },
};
export const pages = createMachine<Page, PageEvent, PageContext>({ id: 'page', initial: getState().tab, strict: true, context: { previous: null }, states });
subscribeStore(() => { const tab = getState().tab; if (pages.get().value !== tab) pages.send({ type: 'SHOW', page: tab }); });

export const currentPage = () => pages.get().value;
export const usePage = () => useSyncExternalStore(pages.subscribe, () => pages.get().value);
