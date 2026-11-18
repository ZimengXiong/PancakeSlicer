import { useSyncExternalStore } from 'react';
import { sidebarHidden, toggleSidebar } from './sidebar';
import { resolvePreset, type State } from './state/store';

export const printerReady = (s: State) => { const r = s.printer ? resolvePreset(s, 'machine', s.printer) : undefined; return !!r?.base && !r.error; };

export { narrowQuery, useNarrow } from './breakpoints';
export const phoneSheetQuery = window.matchMedia('(max-width: 599px), (max-width: 899px) and (max-height: 500px)');
export const overlayRailQuery = window.matchMedia('(max-width: 899px)');
const subscribePlacement = (listener: () => void) => {
  phoneSheetQuery.addEventListener('change', listener);
  overlayRailQuery.addEventListener('change', listener);
  return () => {
    phoneSheetQuery.removeEventListener('change', listener);
    overlayRailQuery.removeEventListener('change', listener);
  };
};
export const usePhoneSheet = () => useSyncExternalStore(subscribePlacement, () => phoneSheetQuery.matches);
export const useOverlayRail = () => useSyncExternalStore(subscribePlacement, () => overlayRailQuery.matches);
const listeners = new Set<() => void>();
export type SettingsSnap = 'peek' | 'half' | 'full';
let snap: SettingsSnap = 'peek';

const subscribe = (l: () => void) => { listeners.add(l); return () => void listeners.delete(l); };

export function setSettingsSheet(next: boolean) {
  setSettingsSnap(next ? (phoneSheetQuery.matches ? 'full' : 'half') : 'peek');
}

export function setSettingsSnap(next: SettingsSnap) {
  if (snap === next) return;
  snap = next;
  listeners.forEach((l) => l());
}

export function showSettings() {
  if (overlayRailQuery.matches) setSettingsSheet(true);
  else if (sidebarHidden()) toggleSidebar(false);
}

export const useSettingsSheet = () => useSyncExternalStore(subscribe, () => snap !== 'peek');
export const useSettingsSnap = () => useSyncExternalStore(subscribe, () => snap);
