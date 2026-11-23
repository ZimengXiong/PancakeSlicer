import { savePreferences } from '../saved-preferences';
import { Dropdown } from './Dropdown';
import { preloadable } from '../preloadable';
import { Check } from './Check';
import { Suspense, useState, useSyncExternalStore } from 'react';
import { markStale, setState, useStore } from '../state/store';
import { setThemePref, subscribeTheme, themePref, type ThemePref } from '../theme';
import { cameraPrefsSnapshot, setCameraPrefs, subscribeCamera, type CameraPrefs } from '../camera';
import { InfoTip } from './InfoTip';
import { Modal, ModalBody, ModalFooter, ModalTitle } from './Modal';
import { AgentPreferences } from './AgentPreferences';

const About = preloadable(() => import('./About'));
const PlateSettings = preloadable(() => import('./PlateSettings'));
const CloneDialog = preloadable(() => import('./CloneDialog'));
const SimplifyDialog = preloadable(() => import('./SimplifyDialog'));
const ObjectTable = preloadable(() => import('./ObjectTable'));
const CreatePrinter = preloadable(() => import('./CreatePrinter'));
const SendDialog = preloadable(() => import('./SendDialog'));
const WhatYouCanDo = preloadable(() => import('./WhatYouCanDo'));
const MixedFilaments = preloadable(() => import('./MixedFilaments'));
const SetupWizard = preloadable(() => import('./FirstRun').then((m) => ({ default: m.SetupWizard })));

const close = () => setState({ appDialog: null });
const mod = /Mac|iPhone|iPad/.test(navigator.platform) ? 'Cmd' : 'Ctrl';

const THEMES: [ThemePref, string][] = [['system', 'System'], ['light', 'Light'], ['dark', 'Dark']];

function setDeveloperMode(developerMode: boolean) {
  savePreferences({'orca.developer_mode':developerMode ? '1' : '0'});
  setState({ developerMode });
}

function setMixedTemperatures(on: boolean) {
  savePreferences({'orca.enable_high_low_temp_mixed_printing':on ? '1' : '0'});
  setState((s) => ({ mixedTemperatures: on, job: markStale(s.job) }));
}

function Preferences() {
  const pref = useSyncExternalStore(subscribeTheme, themePref);
  const camera = useSyncExternalStore(subscribeCamera, cameraPrefsSnapshot);
  const developerMode = useStore((s) => s.developerMode);
  const mixed = useStore((s) => s.mixedTemperatures);
  return (
    <Modal size="m" label="Preferences" onClose={close}>
      <ModalTitle title="Preferences" onClose={close} />
      <ModalBody className="modal-fields">
        <div className="form-row"><span>Theme</span><Dropdown className="combo" aria-label="Theme" value={pref} onValueChange={(next) => setThemePref(next as typeof pref)} options={THEMES.map(([value, label]) => ({ value, label }))} /></div>
        <h3 className="modal-group">Camera</h3>
        <div className="form-row"><span>Mouse controls<InfoTip label="Mouse controls">{'Default: Left-drag to rotate, right- or middle-drag to pan.\nTouchpad: Alt+drag to rotate, Shift+drag to pan.'}</InfoTip></span>
          <Dropdown className="combo" aria-label="Camera style" value={camera.style} onValueChange={(next) => setCameraPrefs({ style: next as CameraPrefs['style'] })} options={[{ value: 'default', label: 'Default' }, { value: 'touchpad', label: 'Touchpad' }]} /></div>
        <label className="form-row"><span>Orbit speed</span>
          <input className="text num" type="number" min={0.05} max={2} step={0.05} value={camera.orbitMult}
            onChange={(e) => { const v = Number(e.target.value); if (v >= 0.05 && v <= 2) setCameraPrefs({ orbitMult: v }); }} /></label>
        <label className="modal-check"><Check checked={camera.zoomToMouse} onCheckedChange={(next) => setCameraPrefs({ zoomToMouse: next })} />Zoom to mouse position</label>
        <label className="modal-check"><Check checked={camera.reverseZoom} onCheckedChange={(next) => setCameraPrefs({ reverseZoom: next })} />Reverse zoom direction</label>
        <h3 className="modal-group">Advanced</h3>
        <label className="modal-check"><Check checked={developerMode} onCheckedChange={(next) => setDeveloperMode(next)} />Developer mode</label>
        {<label className="modal-check"><Check checked={mixed} onCheckedChange={(next) => setMixedTemperatures(next)} />Allow mixed filament temperatures</label>}
        <AgentPreferences />
      </ModalBody>
      <ModalFooter><button type="button" className="btn-primary" onClick={close}>Close</button></ModalFooter>
    </Modal>
  );
}

const SHORTCUTS: [string, [string, string][]][] = [
  ['Global', [
    [`${mod}+N`, 'New project'], [`${mod}+O`, 'Open project'], [`${mod}+S`, 'Save'], [`${mod}+Shift+S`, 'Save as'],
    [`${mod}+I`, 'Import models'], [`${mod}+G`, 'Export'],
    [`${mod}+Z`, 'Undo'], [`${mod}+Y / ${mod}+Shift+Z`, 'Redo'], [`${mod}+F`, 'Search settings'], [`${mod}+P`, 'Preferences'],
    [`${mod}+\\`, 'Hide / show sidebar'], ['Tab', 'Prepare / Preview (in a 3D view)'], ['Shift+Tab / Esc', 'Leave the 3D view'], ['?', 'Shortcuts'],
    ['0 … 6', 'Views: default, top, bottom, front, rear, left, right'], ['I / O', 'Zoom in / out'],
  ]],
  ['Prepare', [
    [`${mod}+A`, 'Select all'], ['Esc', 'Deselect'], [`${mod}+C / ${mod}+X / ${mod}+V`, 'Copy / cut / paste'], [`${mod}+K`, 'Clone'],
    ['Del / Backspace', 'Delete'], [`${mod}+D / ${mod}+Del`, 'Delete all'], ['+ / -', 'Add / remove instance'],
    ['V', 'Printable on / off'], ['F2', 'Rename'], ['A / Shift+A', 'Arrange all / this plate'], ['Q / Shift+Q', 'Orient selected / all'],
    ['Arrows', 'Move 10 mm (Shift: 1 mm)'], ['PgUp / PgDn', 'Rotate 45°'],
    ['M / R / S', 'Move / rotate / scale'], ['F', 'Lay on face'], ['C', 'Cut'], ['U', 'Measure'],
    ['B', 'Boolean'], ['T', 'Text'], ['E', 'Brim ears'], ['L / P / H / N', 'Paint support / seam / fuzzy skin / color'],
    [`${mod}+E`, 'Labels'], [`${mod}+Shift+Enter`, 'Wireframe'],
    ['Left drag', 'Orbit'], ['Right drag', 'Pan'], ['Wheel', 'Zoom'],
  ]],
  ['Painting', [
    ['C / S', 'Circle / sphere'], ['F / G', 'Fill / gap fill'], ['T / H', 'Triangle / height range'], ['1 … 9', 'Filament'],
  ]],
  ['Preview', [
    ['Up / Down', 'Layer up / down'], ['Left / Right', 'Move back / forward'], ['Home / End', 'First / last move'],
    ['L', 'One layer'], ['Shift+G', 'Go to layer'], ['C', 'G-code window'],
  ]],
];

function Keys({ keys }: { keys: string }) {
  return (
    <span className="keys">
      {keys.split(' / ').map((alt, i) => (
        <span key={i} className="keys-alt">
          {i > 0 && <span className="keys-or">/</span>}
          {(alt === '+' ? ['+'] : alt.split(/(?<=.)\+(?=.)/)).map((k) => <kbd key={k}>{k}</kbd>)}
        </span>
      ))}
    </span>
  );
}

function Shortcuts() {
  const [query, setQuery] = useState('');
  const q = query.trim().toLowerCase();
  const tab = useStore((s) => s.tab);
  const pages = [...SHORTCUTS].sort(([a], [b]) => Number(b.toLowerCase() === tab) - Number(a.toLowerCase() === tab))
    .map(([page, rows]) => [page, rows.filter(([k, what]) => !q || what.toLowerCase().includes(q) || k.toLowerCase().includes(q))] as const)
    .filter(([, rows]) => rows.length);
  return (
    <Modal size="xl" className="shortcuts-dialog" label="Keyboard shortcuts" onClose={close} backdropClose>
      <ModalTitle title="Keyboard shortcuts" onClose={close} />
      <div className="shortcuts-search">
        <input className="text" type="search" placeholder="Filter" aria-label="Filter shortcuts" value={query} data-autofocus onChange={(e) => setQuery(e.target.value)} />
      </div>
      <div className="shortcuts-body" tabIndex={0}>
        {pages.map(([page, rows]) => (
          <section key={page}>
            <h3>{page}</h3>
            <table className="shortcuts-table">
              <tbody>
                {rows.map(([key, what]) => (
                  <tr key={key + what}><th scope="row"><Keys keys={key} /></th><td>{what}</td></tr>
                ))}
              </tbody>
            </table>
          </section>
        ))}
        {!pages.length && <div className="shortcuts-none">No match</div>}
      </div>
    </Modal>
  );
}

export default function AppDialog({ which }: { which: 'about' | 'preferences' | 'shortcuts' | 'send' | 'setup' | 'what' | 'plate' | 'clone' | 'simplify' | 'objectTable' | 'createPrinter' | 'mixed' }) {
  if (which === 'mixed') return <Suspense fallback={null}><MixedFilaments close={close} /></Suspense>;
  if (which === 'createPrinter') return <Suspense fallback={null}><CreatePrinter close={close} /></Suspense>;
  if (which === 'objectTable') return <Suspense fallback={null}><ObjectTable close={close} /></Suspense>;
  if (which === 'simplify') return <Suspense fallback={null}><SimplifyDialog close={close} /></Suspense>;
  if (which === 'clone') return <Suspense fallback={null}><CloneDialog close={close} /></Suspense>;
  if (which === 'plate') return <Suspense fallback={null}><PlateSettings close={close} /></Suspense>;
  if (which === 'what') return <Suspense fallback={null}><WhatYouCanDo close={close} /></Suspense>;
  if (which === 'send') return <Suspense fallback={null}><SendDialog close={close} /></Suspense>;
  if (which === 'setup') return <Suspense fallback={null}><SetupWizard close={close} /></Suspense>;
  if (which === 'preferences') return <Preferences />;
  if (which === 'shortcuts') return <Shortcuts />;
  return <Suspense fallback={null}><About close={close} /></Suspense>;
}
