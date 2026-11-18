import { createMachine } from './fsm';
export type ProjectRoute = { kind: 'blank' } | { kind: 'projects' } | { kind: 'project'; id: string } | { kind: 'missing' };
const hashed = () => location.protocol === 'file:' || import.meta.env.MODE === 'demo';
export const projectHref = (id: string) => `${hashed() ? '#' : ''}/p/${encodeURIComponent(id)}`;
export const projectsHref = () => `${hashed() ? '#' : ''}/projects`;
function parseRoute(): ProjectRoute {
  const path = hashed() ? location.hash.slice(1) || '/' : location.pathname;
  if (path === '/') return { kind: 'blank' };
  if (path === '/projects' || path === '/projects/') return { kind: 'projects' };
  const match = /^\/p\/([^/]+)\/?$/.exec(path);
  if (!match) return { kind: 'missing' };
  try { return { kind: 'project', id: decodeURIComponent(match[1]) }; } catch { return { kind: 'missing' }; }
}
export const urlRoute = (): ProjectRoute => parseRoute();
export const urlProject = (): string | null => { const r = parseRoute(); return r.kind === 'project' ? r.id : null; };
export function writeRoute(path: string, replace = false) {
  const href = `${hashed() ? '#' : ''}${path}`;
  const current = hashed() ? location.hash : location.pathname;
  if (href === current) return;
  if (replace) history.replaceState(null, '', href);
  else history.pushState(null, '', href);
  currentRoute();
}

type RouteEvent = { type: 'NAVIGATE'; route: ProjectRoute };
const transitions = [
  { target: 'project', guard: (_: { id: string | null }, e: RouteEvent) => e.route.kind === 'project', action: (_: { id: string | null }, e: RouteEvent) => ({ id: e.route.kind === 'project' ? e.route.id : null }) },
  { target: 'projects', guard: (_: { id: string | null }, e: RouteEvent) => e.route.kind === 'projects', action: () => ({ id: null }) },
  { target: 'blank', guard: (_: { id: string | null }, e: RouteEvent) => e.route.kind === 'blank', action: () => ({ id: null }) },
  { target: 'missing', action: () => ({ id: null }) },
] as const;
const routeOn = { NAVIGATE: [...transitions] };
export const routing = createMachine<ProjectRoute['kind'], RouteEvent, { id: string | null }>({
  id: 'route', initial: 'blank', context: { id: null },
  states: { blank: { on: routeOn }, projects: { on: routeOn }, project: { on: routeOn }, missing: { on: routeOn } },
});
export function currentRoute(): ProjectRoute {
  const route = parseRoute(), current = routing.get();
  if (route.kind !== current.value || (route.kind === 'project' && route.id !== current.context.id))
    routing.send({ type: 'NAVIGATE', route });
  return route;
}
window.addEventListener('popstate', currentRoute);
window.addEventListener('hashchange', currentRoute);
