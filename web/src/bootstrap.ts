import { checkBrowserSupport, showBrowserNotice, unsupportedBrowserMessage } from './browser-support';
import './theme';
document.documentElement.classList.add('orca-parity');
import './styles/tokens.css';
import './styles/primitives.css';
import './app.css';
import './mobile.css';
import './components/controls.css';
import { registerServiceWorker } from './sw';

const root = document.getElementById('root')!;
registerServiceWorker();
window.addEventListener('load', () => void import('./build-update').then((m) => m.watchForNewBuild()), { once: true });
if (typeof Worker !== 'undefined') void import('./api/worker').then((m) => m.settingsWorker()).catch(() => {});
const main = import('./main');
main.catch(() => {});
checkBrowserSupport().then(() => {
  main.then((m) => m.start(), () => showBrowserNotice(root,
    'Oops! PancakeSlicer couldn\'t finish loading. Check your internet connection and reload the page.', 'Oops! Couldn\'t start PancakeSlicer'));
}, (error: unknown) => showBrowserNotice(root,
  unsupportedBrowserMessage(error instanceof Error ? error.message : String(error))));
