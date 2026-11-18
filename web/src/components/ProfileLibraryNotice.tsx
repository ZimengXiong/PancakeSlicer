import { useEffect } from 'react';
import { librarySync } from '../flow/library-sync';
import { loadCatalog } from '../actions';
import { getState, useStore } from '../state/store';
import { getRecord } from '../profiles/records';
import { OPTIONS } from '../options';
import { display } from '../profiles/format';
import type { Config } from '../api/types';
import { CLIENT_OUTDATED } from '../persist/profile-library';
import { useMachine } from '../flow/fsm';
import { libraryWrites } from '../profiles/library-writes';

function conflictText(keys: string[], intents: readonly { change?: { base: Map<string, unknown>; next: Map<string, unknown> } }[]): string {
  const maps = ['settings', 'values', 'machine', 'printOverrides'];
  return keys.map((k) => {
    const intent = intents.find((i) => i.change?.next.has(k)), latest = getRecord(getState().records, k) as Record<string, unknown> | undefined;
    const base = intent?.change?.base.get(k) as Record<string, unknown> | undefined, next = intent?.change?.next.get(k) as Record<string, unknown> | undefined;
    const name = (latest?.name ?? next?.name ?? '') as string, moves: string[] = [];
    for (const field of maps) {
      const b = (base?.[field] ?? {}) as Config, n = (next?.[field] ?? {}) as Config, l = (latest?.[field] ?? {}) as Config;
      for (const key of Object.keys({ ...b, ...n })) {
        const same = (x: unknown, y: unknown) => JSON.stringify(x) === JSON.stringify(y);
        if (!same(b[key], n[key]) && !same(b[key], l[key]) && !same(n[key], l[key])) moves.push(`${OPTIONS[key]?.label || key} ${display(key, n[key])} → ${display(key, l[key])}`);
      }
    }
    return moves.length ? `${name}: ${moves.join(', ')}` : name;
  }).filter(Boolean).join('; ');
}
const conflictNames = (keys: string[]) => [...new Set(keys.map((k) => (getRecord(getState().records, k) as { name?: string } | undefined)?.name ?? '').filter(Boolean))].join(', ');

export function LibrarySaveStatus() {
  const { value, context } = useMachine(libraryWrites);
  const live = useMachine(librarySync, (m) => m.value === 'live');
  if (!live && (value === 'idle' || value === 'saved')) return <div className="library-save-status" data-library-save="reconnecting" role="status" aria-live="polite">Reconnecting…</div>;
  if (value === 'idle') return null;
  const send = (type: 'RETRY' | 'REAPPLY' | 'DISCARD' | 'DISMISS') => () => libraryWrites.send({ type });
  const button = (label: string, type: 'RETRY' | 'REAPPLY' | 'DISCARD' | 'DISMISS') => <button type="button" className="btn" onClick={send(type)}>{label}</button>;
  const what = [...new Set(context.inflight?.intents.map((i) => i.label) ?? [])].join(', ');
  const names = [...new Set([...context.inflight?.intents ?? [], ...context.queued].flatMap((i) => [...i.change?.next.entries() ?? []]
    .map(([key, next]) => (next as { name?: string } | undefined)?.name ?? (getRecord(getState().records, key) as { name?: string } | undefined)?.name ?? '')).filter(Boolean))];
  const named = names.length ? ` ${names.slice(0, 3).join(', ')}${names.length > 3 ? ` and ${names.length - 3} more` : ''}` : '';
  const labels = new Set([...context.inflight?.intents ?? [], ...context.queued].map((i) => i.label));
  const one = labels.size === 1 ? [...labels][0] : '';
  const verb = ({ Remove: 'remove', Restore: 'restore', Import: 'import', Duplicate: 'duplicate', 'Add profile': 'add' } as Record<string, string>)[one] ?? 'save';
  if (value === 'writing' || value === 'saved' || value === 'held') return <div className="library-save-status" data-library-save={value} role="status" aria-live="polite">{value === 'writing' ? 'Saving…' : value === 'held' ? 'Hang tight — saving as soon as your profiles finish loading…' : 'Saved'}</div>;
  return <div className="profile-library-notice" data-library-save={value} role="alert">
    {value === 'offline' && <div><strong>Couldn't {verb}{named}: can't reach the service right now. {verb === 'save' ? 'Your changes are kept and save' : `It is kept and goes through`} when it's back.</strong>{button('Retry', 'RETRY')}</div>}
    {value === 'failed' && <div><strong>{context.plain ? context.error : `Oops! Couldn't ${verb}${named || (what ? ` (${what})` : '')}: ${context.error}`}</strong>
      {context.inflight ? <>{button('Retry', 'RETRY')}{button('Discard', 'DISCARD')}</> : button('Dismiss', 'DISMISS')}</div>}
    {value === 'conflicted' && <div><strong>Changed elsewhere: {conflictText(context.conflicts, context.inflight?.intents ?? []) || conflictNames(context.conflicts) || 'this profile'} (saved in another tab or device first).</strong>
      {button('Reapply on latest', 'REAPPLY')}{button('Discard', 'DISCARD')}</div>}
  </div>;
}

export function ProfileLibraryNotice() {
  const load = useStore(s => s.profileLibraryLoad);
  const error = load.state === 'failed' ? load.error : null;
  const notices = useStore(s => s.libraryHealth.notices);
  useEffect(() => { if (error && error !== CLIENT_OUTDATED) console.error('Profile library could not be read:', error); }, [error]);
  const said = notices.map((n) => <div key={n.code} className="profile-library-notice" role="alert" data-library-notice={n.code}><div><strong>{n.message}</strong></div></div>);
  if (!error) return <>{said}<LibrarySaveStatus/></>;
  if (error === CLIENT_OUTDATED) return <>{said}<div className="profile-library-notice" role="alert">
    <div><strong>PancakeSlicer got an update!</strong><button type="button" className="btn" onClick={() => location.reload()}>Reload</button></div>
  </div></>;
  return <>{said}<div className="profile-library-notice" role="alert">
    <div><strong>Oops! Couldn't read your profile library</strong><button type="button" className="btn" onClick={() => void loadCatalog()}>Retry</button></div>
  </div></>;
}
