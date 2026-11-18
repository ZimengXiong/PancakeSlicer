import { observePreference, preferenceText, savedPreferencesSnapshot, savePreferences, subscribeSavedPreferences } from './saved-preferences';
import { useSyncExternalStore } from 'react';
import { lastRailWidth } from './components/rail-layout';
import { bootShape } from './boot-shape';

const KEY = 'orca.web.sidebar';
const listeners = new Set<() => void>();
let hidden = false;


export const sidebarHidden = () => hidden;

export function toggleSidebar(next = !hidden) {
  hidden = next;
  savePreferences({[KEY]: hidden ? 'hidden' : null});
  listeners.forEach((l) => l());
}

const subscribe = (l: () => void) => { listeners.add(l); return () => void listeners.delete(l); };
export const useSidebarHidden = () => useSyncExternalStore(subscribe, sidebarHidden);

export const RAIL_MIN = 320, railMax = () => Math.round(window.innerWidth * 0.45);
const WIDTH_KEY = 'orca.web.railWidth';
const clampRail = (w: number) => Math.round(Math.min(Math.max(RAIL_MIN, railMax()), Math.max(RAIL_MIN, w)));
let width: number | null = ((w) => w === null ? null : clampRail(w))(((last) => last === undefined ? bootShape()?.railWidth ?? null : last)(lastRailWidth()));

export function setRailWidth(next: number | null, persist = true) {
  width = next === null ? null : clampRail(next);
  if (persist) savePreferences({[WIDTH_KEY]: width});
  listeners.forEach((l) => l());
}
export const useRailWidth = () => useSyncExternalStore(subscribe, () => width);
export const railWidthNow = () => width;

export type RailProfile = 'machine' | 'filament' | 'process';
let profile: RailProfile = 'process';
export const setRailProfile = (next: RailProfile) => { profile = next; listeners.forEach((l) => l()); };
export const useRailProfile = () => useSyncExternalStore(subscribe, () => profile);

export const OBJECTS_ROW = 24, OBJECTS_MIN = OBJECTS_ROW * 2, OBJECTS_MAX = OBJECTS_ROW * 12, OBJECTS_DEFAULT = OBJECTS_ROW * 4;
const OBJECTS_KEY = 'orca.web.railObjects';
let objectsHeight = OBJECTS_DEFAULT;

export function setObjectsHeight(next: number, persist = true) {
  objectsHeight = Math.round(Math.min(OBJECTS_MAX, Math.max(OBJECTS_MIN, next)));
  if (persist) savePreferences({[OBJECTS_KEY]: objectsHeight});
  listeners.forEach((l) => l());
}
export const useObjectsHeight = () => useSyncExternalStore(subscribe, () => objectsHeight);

observePreference(KEY, () => { hidden = preferenceText(KEY) === 'hidden'; listeners.forEach(l => l()); });
const followWidth = () => {
  if (!savedPreferencesSnapshot().hydrated) return;
  const saved = Number(preferenceText(WIDTH_KEY)) || null;
  if ((saved === null ? null : clampRail(saved)) !== width) setRailWidth(saved, false);
};
observePreference(WIDTH_KEY, followWidth);
subscribeSavedPreferences(followWidth);
observePreference(OBJECTS_KEY, () => setObjectsHeight(Number(preferenceText(OBJECTS_KEY)) || OBJECTS_DEFAULT, false));
