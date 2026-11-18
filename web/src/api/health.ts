const FRESH_MS = 2000;
let last: { read: Promise<Response>; at: number } | null = null;


export function readHealth(timeoutMs: number): Promise<Response> {
  if (!last || performance.now() - last.at > FRESH_MS) {
    const read = fetch('/api/healthz', { signal: AbortSignal.timeout(timeoutMs) });
    const entry = { read, at: performance.now() };
    last = entry;
    void read.then((r) => { if (r.ok) entry.at = performance.now(); else if (last === entry) last = null; }, () => { if (last === entry) last = null; });
  }
  return last.read.then((r) => r.clone());
}
