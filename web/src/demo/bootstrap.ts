import { configureDemo, installDemoTransport } from './transport';
installDemoTransport();
configureDemo({
  notice: code => { void import('../state/store').then(m => m.toast(code)); },
  resolve: async (...args) => (await import('./resolver')).demoAnswer(...args),
  slotOffer: async (...args) => (await import('./resolver')).demoSlotOffer(...args),
});
void import('../bootstrap');
void import('./sample-slice').then((m) => m.showSampleSlice());
document.addEventListener('click', (event) => {
  if (!(event.target as Element | null)?.closest('[data-output]')) return;
  event.preventDefault(); event.stopImmediatePropagation();
  void import('../state/store').then(({ toast }) => toast('not_available_demo_mode'));
}, true);
import './demo.css';
