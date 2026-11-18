const SCRIPT = '/orca-sw.js';

const unregister = () => {
  void navigator.serviceWorker.getRegistrations().then((all) => Promise.all(all.map((r) => r.unregister())));
  void caches.keys().then((names) => Promise.all(names.filter((n) => n.startsWith('orca-immutable-')).map((n) => caches.delete(n))));
};

export function registerServiceWorker(): void {
  if (import.meta.env.MODE === 'demo') return;
  if (!('serviceWorker' in navigator) || !window.isSecureContext) return;
  if (import.meta.env.DEV) return unregister();
  const param = new URLSearchParams(location.search).get('sw');
  try {
    if (param === 'off' || param === 'on') localStorage.setItem('orca.sw', param);
  } catch {  }
  let off = param === 'off';
  try { off ||= localStorage.getItem('orca.sw') === 'off'; } catch {  }
  if (off) return unregister();
  const register = () => void navigator.serviceWorker.register(SCRIPT, { scope: '/' }).catch(() => {});
  if (document.readyState === 'complete') register();
  else window.addEventListener('load', register, { once: true });
}
