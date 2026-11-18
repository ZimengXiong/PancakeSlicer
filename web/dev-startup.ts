import { createHash } from 'node:crypto';
import path from 'node:path';
import type { DevEnvironment, HtmlTagDescriptor, Plugin } from 'vite';
import { parseAst } from 'vite';

const STARTUP_MODULES = [
  '/@vite/client', '/src/bootstrap.ts', '/src/main.tsx', '/src/persist/preferences.ts',
  '/src/plater/Plater3D.tsx', '/src/components/WorkspaceRail.tsx', '/src/components/Navigator.tsx', '/src/projects/ProjectStatus.tsx',
  '/src/plater/orientPrompt.ts', '/src/profiles/catalog-startup.ts', '/src/preflight/marks.ts',
  '/src/profiles/library-hydration.ts', '/src/profiles/printer-setups.ts', '/src/flow/project-recovery.ts',
  '/src/persist/drafts.ts', '/src/flow/project-journal.ts', '/src/persist/project-service.ts',
  '/src/persist/wire-worker.ts', '/src/flow/boot.ts',
];

const LATE_STARTUP = new Set(['src/plater/Plater3D.tsx', 'src/components/Navigator.tsx', 'src/preflight/marks.ts', 'src/plater/orientPrompt.ts']);

const TIMESTAMP = /([?&])t=\d+&?/;
const IMPORT = /([?&])import&?/;
const strip = (url: string, re: RegExp) => url.replace(re, '$1').replace(/[?&]$/, '');
const moduleId = (url: string) => {
  const plain = strip(strip(url, TIMESTAMP), IMPORT);
  return plain.startsWith('/@id/') ? plain.slice(5).replace('__x00__', '\0') : plain;
};

const parsed = new WeakMap<object, string[]>();
function staticImports(result: { code: string }) {
  let urls = parsed.get(result);
  if (!urls) {
    urls = [];
    for (const node of parseAst(result.code).body) {
      if ((node.type === 'ImportDeclaration' || node.type === 'ExportAllDeclaration' || node.type === 'ExportNamedDeclaration') &&
        node.source && typeof node.source.value === 'string') urls.push(node.source.value);
    }
    parsed.set(result, urls);
  }
  return urls;
}

function served(env: DevEnvironment, url: string) {
  const time = env.moduleGraph.urlToModuleMap.get(url)?.lastHMRTimestamp;
  if (!time) return url;
  const [path, query] = url.split('?');
  return `${path}?t=${time}${query ? `&${query}` : ''}`;
}

async function staticGraph(env: DevEnvironment, roots: string[]) {
  const seen = new Set<string>();
  const order: string[] = [];
  let level = roots;
  while (level.length) {
    const next: string[] = [];
    await Promise.all(level.map(async (url) => {
      if (seen.has(url)) return;
      seen.add(url);
      const result = await env.transformRequest(moduleId(url)).catch(() => null);
      if (!result) return;
      order.push(url);
      try {
        next.push(...staticImports(result).filter((dep) => dep.startsWith('/') && !seen.has(dep)));
      } catch {  }
    }));
    level = next;
  }
  return order;
}

async function flatWorker(env: DevEnvironment, url: string) {
  const result = await env.transformRequest(moduleId(url));
  if (!result) return null;
  const own = new Set(staticImports(result));
  const extra = (await staticGraph(env, [url])).slice(1).filter((dep) => !own.has(dep));
  const map = result.map && 'version' in result.map && result.map.mappings
    ? `//# sourceMappingURL=data:application/json;base64,${Buffer.from(JSON.stringify(result.map)).toString('base64')}\n` : '';
  return `${result.code}\n${extra.map((dep) => `import ${JSON.stringify(dep)};\n`).join('')}${map}`;
}

export function devStartup(): Plugin {
  let env: DevEnvironment | undefined, api: string | undefined;
  const bootShape = async (page: string) => {
    if (!api) return null;
    try {
      const r = await fetch(`${api}/api/boot?page=${encodeURIComponent(page)}`, { signal: AbortSignal.timeout(1000) });
      const text = r.ok ? await r.text() : '';
      return text && text.trim() !== 'null' && !text.includes('<') ? text.trim() : null;
    } catch { return null; }
  };
  return {
    name: 'orca-dev-startup',
    apply: 'serve',
    config: () => ({ server: { warmup: { clientFiles: STARTUP_MODULES.filter((url) => url.startsWith('/src/')).map((url) => `.${url}`) } } }),
    configureServer(server) {
      env = server.environments.client;
      server.middlewares.use((req, res, next) => {
        if (!env || req.method !== 'GET' || !req.url?.includes('worker_file')) return next();
        flatWorker(env, req.url).then((code) => {
          if (code === null) return next();
          const etag = `W/"${createHash('sha1').update(code).digest('base64url')}"`;
          if (req.headers['if-none-match'] === etag) { res.statusCode = 304; res.end(); return; }
          res.writeHead(200, { 'Content-Type': 'text/javascript', 'Cache-Control': 'no-cache', ETag: etag });
          res.end(code);
        }, next);
      });
    },
    transform: {
      order: 'post',
      handler(code, id) {
        if (env?.depsOptimizer?.isOptimizedDepFile(id)) return { code: `${code}\n`, map: { mappings: '' } };
      },
    },
    configResolved(config) {
      const target = config.server.proxy?.['/api'];
      api = typeof target === 'string' ? target : typeof target?.target === 'string' ? target.target : undefined;
    },
    async transformIndexHtml(_html, ctx) {
      if (!env) return [];
      const [urls, boot] = await Promise.all([staticGraph(env, STARTUP_MODULES.map((url) => served(env!, url))), bootShape(ctx.originalUrl ? new URL(ctx.originalUrl, 'http://page').pathname : ctx.path)]);
      return [...urls.map((href): HtmlTagDescriptor => ({ tag: 'link', attrs: { rel: 'modulepreload', href }, injectTo: 'head' })),
        ...(boot ? [{ tag: 'script', attrs: { id: 'pancake-boot', type: 'application/json' }, children: boot, injectTo: 'head' } as HtmlTagDescriptor] : [])];
    },
  };
}

const DEFERRED_PACKAGES = /[\\/]@radix-ui[\\/]react-(tooltip|hover-card)[\\/]/;
const bootIds = new Set<string>(), startupIds = new Set<string>(), lateIds = new Set<string>();
export const startupGroups = [
  { name: 'boot', test: (id: string) => bootIds.has(id) || id.startsWith('\0'), includeDependenciesRecursively: false },
  { name: 'startup', test: (id: string) => startupIds.has(id), includeDependenciesRecursively: false },
  { name: 'startup-late', test: (id: string) => lateIds.has(id), includeDependenciesRecursively: false },
];

export function buildStartup(): Plugin {
  let root = '', base = '/';
  return {
    name: 'orca-build-startup',
    apply: 'build',
    configResolved(config) { root = config.root; base = config.base; },
    buildEnd() {
      const closure = (from: string[]) => {
        const seen = new Set<string>(), todo = [...from];
        while (todo.length) {
          const id = todo.pop()!;
          if (seen.has(id) || DEFERRED_PACKAGES.test(id)) continue;
          const info = this.getModuleInfo(id);
          if (!info) continue;
          seen.add(id);
          todo.push(...info.importedIds);
        }
        return seen;
      };
      const ids = STARTUP_MODULES.filter((url) => url.startsWith('/src/')).map((url) => path.join(root, url));
      const isLate = (id: string) => LATE_STARTUP.has(path.relative(root, id).replaceAll(path.sep, '/'));
      const bootstrap = path.join(root, 'src/bootstrap.ts'), entry = closure([bootstrap]);
      const early = closure(ids.filter((id) => !isLate(id) && !entry.has(id)));
      bootIds.clear(); startupIds.clear(); lateIds.clear();
      for (const id of entry) if (id !== bootstrap) bootIds.add(id);
      for (const id of early) if (!entry.has(id)) startupIds.add(id);
      const renderer = (id: string) => /[\\/]three[\\/]build[\\/]three\.module\.js$/.test(id);
      for (const id of closure(ids.filter(isLate))) if (!entry.has(id) && (!early.has(id) || renderer(id))) lateIds.add(id);
      for (const id of lateIds) startupIds.delete(id);
      startupIds.delete(path.join(root, 'src/main.tsx'));
    },
    transformIndexHtml: {
      order: 'post',
      handler(_html, { bundle, chunk: entry }) {
        if (!bundle) return [];
        const startup = new Set(STARTUP_MODULES.map((url) => path.join(root, url)));
        const chunks = new Map(Object.values(bundle).flatMap((c) => c.type === 'chunk' ? [[c.fileName, c] as const] : []));
        const seen = new Set<string | undefined>();
        const vite = (name: string | undefined) => {
          if (seen.has(name)) return;
          seen.add(name);
          if (name) chunks.get(name)?.imports.forEach(vite);
        };
        vite(entry?.fileName);
        const files: string[] = [], styles = new Set<string>();
        const visit = (name: string) => {
          const c = chunks.get(name);
          if (!c || seen.has(name)) return;
          seen.add(name);
          files.push(name);
          c.viteMetadata?.importedCss.forEach((css) => styles.add(css));
          c.imports.forEach(visit);
        };
        const startupChunks = [...chunks.values()].filter((c) => c.moduleIds.some((id) => startup.has(id)));
        const deferred = (c: { moduleIds: string[] }) => c.moduleIds.some((id) => LATE_STARTUP.has(path.relative(root, id).replaceAll(path.sep, '/')));
        for (const c of startupChunks) if (!deferred(c)) visit(c.fileName);
        return [
          ...files.map((file): HtmlTagDescriptor => ({ tag: 'link', attrs: { rel: 'modulepreload', crossorigin: true, href: base + file }, injectTo: 'head' })),
          ...[...styles].map((file): HtmlTagDescriptor => ({ tag: 'link', attrs: { rel: 'preload', as: 'style', crossorigin: true, href: base + file }, injectTo: 'head' })),
        ];
      },
    },
  };
}
