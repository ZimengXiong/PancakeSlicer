import { useSyncExternalStore } from 'react';
import { initialiseSavedPreferences, savedPreferencesSnapshot, subscribeSavedPreferences } from '../saved-preferences';
import { inNoticeLane, useNoticeLane, useTokenAsked } from './Notices';
export default function SavedPreferencesNotice() {
  const saved = useSyncExternalStore(subscribeSavedPreferences, savedPreferencesSnapshot);
  const token = useTokenAsked(), failed = saved.state === 'failed' && !token;
  const lane = useNoticeLane(failed);
  if (!failed) return null;
  const what = saved.hydrated ? 'Oops! Couldn\'t save your preferences' : 'Oops! Couldn\'t load your preferences';
  return inNoticeLane(<div className="toast toast-error" role="alert"><div className="toast-text"><div>{saved.unreachable || !saved.error ? `${what} — can't reach the service right now.` : `${what}: ${saved.error}`}</div></div><button type="button" className="btn" onClick={() => void initialiseSavedPreferences(true).catch(() => {})}>Retry</button></div>, lane);
}
