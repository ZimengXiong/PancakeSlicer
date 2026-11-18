let worker: Worker | null = null;
let id = 0;
const failures = new Set<(error: Error) => void>();

export function settingsWorker() {
  if (worker) return worker;
  const w = new Worker(new URL('./settings.worker.ts', import.meta.url), { type: 'module' });
  const fail = (message: string) => {
    if (worker === w) worker = null;
    w.terminate();
    const error = new Error(message);
    for (const f of [...failures]) f(error);
  };
  w.addEventListener('error', (e) => { e.preventDefault(); fail(e.message || 'Oops! The settings worker stopped'); });
  w.addEventListener('messageerror', () => fail('Settings worker message unreadable'));
  return (worker = w);
}
export function workerId() { return ++id; }

export function onWorkerFailure(fail: (error: Error) => void): () => void {
  failures.add(fail);
  return () => void failures.delete(fail);
}
