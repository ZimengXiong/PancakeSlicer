import { getState, subscribeStore, type State } from '../state/store';
import { stripRecord } from './material-library';
import { adoptingLibrary, commitRecords } from './transactions';

export function saveSelectionStrip(s: State = getState()): void {
  if (s.profileLibraryLoad.state !== 'ready') return;
  const record = stripRecord(s);
  if (record) commitRecords([{ kind: 'printer', record }], { label: 'Printer setup', optimistic: true }).catch(() => {});
}

let watching = false;
export function watchPrinterSetups(): void {
  if (watching) return;
  watching = true;
  let previous = getState();
  subscribeStore(() => {
    const current = getState(), before = previous;
    previous = current;
    if (adoptingLibrary() || before.profileLibraryLoad.state !== 'ready' || current.projectPresets !== before.projectPresets || current.profileSnapshots !== before.profileSnapshots) return;
    if (current.printer === before.printer && current.process === before.process) return;
    saveSelectionStrip(current);
  });
}
