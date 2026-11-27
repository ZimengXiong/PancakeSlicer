import http from 'node:http';

const AGENT_ROUTES = [
  /^GET \/api\/(?:healthz|library|events|printer-connections)$/,
  /^POST \/api\/(?:resolve|jobs|blobs\/missing)$/,
  /^PUT \/api\/blobs\/[0-9a-f]{64}$/,
  /^GET \/api\/jobs\/[^/]+(?:\/(?:download|send|send\/plan))?$/,
  /^POST \/api\/jobs\/[^/]+\/cancel$/,
];

const PRINTER_ROUTES = [
  /^POST \/api\/jobs\/[^/]+\/send$/,
  /^POST \/api\/printer-connections\/[^/]+\/(?:device|control)$/,
  /^GET \/api\/printer-connections\/[^/]+\/camera(?:\/[^/]+)?$/,
];

export function agentApiPermission(method: string, target: string): 'agent' | 'printer' | undefined {
  const pathname = target.split('?', 1)[0];
  if (!pathname.startsWith('/api/') || /[\s#\\]/.test(target)) return;
  try {
    if (pathname.split('/').slice(1).some((segment) => {
      const decoded = decodeURIComponent(segment);
      return !decoded || decoded === '.' || decoded === '..' || /[/\\]/.test(decoded);
    })) return;
  } catch {
    return;
  }
  const route = `${method} ${pathname}`;
  if (AGENT_ROUTES.some((pattern) => pattern.test(route))) return 'agent';
  if (PRINTER_ROUTES.some((pattern) => pattern.test(route))) return 'printer';
}

export function forwardAgentApi(req: http.IncomingMessage, res: http.ServerResponse, service: string, serviceToken: string, unavailable: () => void) {
  const headers: http.OutgoingHttpHeaders = { authorization: `Bearer ${serviceToken}` };
  for (const name of ['content-type', 'content-length', 'accept']) {
    if (req.headers[name] !== undefined) headers[name] = req.headers[name];
  }
  const upstream = http.request(service, { method: req.method, path: req.url, headers }, (response) => {
    const headers: http.OutgoingHttpHeaders = {};
    for (const name of ['content-type', 'content-length', 'content-disposition', 'cache-control']) {
      if (response.headers[name] !== undefined) headers[name] = response.headers[name];
    }
    res.writeHead(response.statusCode ?? 502, headers);
    response.on('error', (error) => res.destroy(error));
    response.pipe(res);
  });
  upstream.on('error', () => {
    if (res.destroyed) return;
    if (res.headersSent) res.destroy();
    else unavailable();
  });
  res.on('close', () => upstream.destroy());
  req.pipe(upstream);
}
