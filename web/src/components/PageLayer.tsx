import { createContext, useContext, useEffect, useRef, useState, type ComponentType, type ReactNode } from 'react';
import { createPortal } from 'react-dom';
import { onLeavePage, usePage, type Page } from '../flow/page';

const Layer = createContext<HTMLElement | undefined>(undefined);
const PageOf = createContext<Page | undefined>(undefined);

export function PageScope({ page, children }: { page: Page; children: ReactNode }) {
  const shown = usePage() === page;
  const [host] = useState(() => { const el = document.createElement('div'); el.className = 'page-layer'; el.dataset.page = page; return el; });
  useEffect(() => { document.body.appendChild(host); return () => host.remove(); }, [host]);
  useEffect(() => { host.style.display = shown ? 'contents' : 'none'; }, [host, shown]);
  return <PageOf.Provider value={page}><Layer.Provider value={host}>{children}</Layer.Provider></PageOf.Provider>;
}

export const useOverlayHost = () => useContext(Layer);
export function PagePortal({ children }: { children: ReactNode }) {
  return createPortal(children, useContext(Layer) ?? document.body);
}

type PortalLike = ComponentType<{ container?: HTMLElement | null; forceMount?: true; children?: ReactNode }>;
export function InLayer({ portal: P, forceMount, children }: { portal: PortalLike; forceMount?: true; children: ReactNode }) {
  return <P container={useContext(Layer)} forceMount={forceMount}>{children}</P>;
}

export function usePageLeave(fn: () => void) {
  const page = useContext(PageOf), latest = useRef(fn);
  latest.current = fn;
  useEffect(() => (page ? onLeavePage(page, () => latest.current()) : undefined), [page]);
}
