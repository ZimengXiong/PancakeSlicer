import { useEffect } from 'react';

let pending: 'toggle' | 'open' | null = null;
const listeners = new Set<() => void>();

export function requestAddSettings(mode: 'toggle' | 'open' = 'toggle') {
  pending = mode;
  listeners.forEach((l) => l());
}

export function useAddSettingsRequest(ready: boolean, open: (mode: 'toggle' | 'open') => void) {
  useEffect(() => {
    if (!ready) return;
    const run = () => { if (pending) { const mode = pending; pending = null; open(mode); } };
    run();
    listeners.add(run);
    return () => void listeners.delete(run);
  }, [ready]); // eslint-disable-line react-hooks/exhaustive-deps
}
