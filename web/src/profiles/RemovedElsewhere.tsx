import type { PresetType } from '../api/types';
import { getState, setState, toast, useStore } from '../state/store';
import { recordOf } from './view';
import { commitRecords } from './transactions';
import type { AnyRecord } from './records';
import { profileSaveFor } from '../flow/profile-save';
import { useMachine } from '../flow/fsm';
import { changedKeys } from './model';

const kindOf = (type: PresetType) => type === 'machine' ? 'printer' as const : type;

export function RemovedElsewhere({ type, onRestored }: { type: PresetType; onRestored(key: string): void }) {
  const drafts = useStore((s) => s.libraryDrafts), records = useStore((s) => s.records);
  void records;
  const removed = Object.entries(drafts).flatMap(([k, draft]) => {
    const [t, key] = JSON.parse(k) as [PresetType, string];
    if (t !== type) return [];
    const record = recordOf(getState(), t, key);
    if (!draft.origin || record && !record.archived) return [];
    return [{ k, key, name: record?.name ?? draft.origin.name, record: record ?? draft.origin }];
  });
  if (!removed.length) return null;
  const drop = (k: string) => setState((s) => { const next = { ...s.libraryDrafts }; delete next[k]; return { libraryDrafts: next }; });
  const discard = (k: string) => { drop(k); const m = profileSaveFor(k); if (m.can({ type: 'CANCEL' })) m.send({ type: 'CANCEL' }); };
  const restore = async (r: (typeof removed)[number]) => {
    const { archived: _a, ...rest } = r.record as AnyRecord & { archived?: unknown };
    try {
      await commitRecords([{ kind: kindOf(type), record: rest as AnyRecord }], { label: 'Restore' });
      const d = getState().libraryDrafts[r.k];
      if (d && !Object.keys(changedKeys(d.values, d.baseline)).length && !d.cleared?.length) drop(r.k);
      onRestored(r.key);
    }
    catch (e) { toast('removedelsewhere_failed', { detail: e instanceof Error ? e.message : String(e) }); }
  };
  return <div className="profile-library-notice" role="alert" data-removed-elsewhere>
    {removed.map((r) => <div key={r.k}><strong>{r.name} was removed in another window. <Kept draftKey={r.k}/></strong>
      <button type="button" className="btn" onClick={() => void restore(r)}>Restore &amp; keep editing</button>
      <button type="button" className="btn-ghost" onClick={() => discard(r.k)}>Discard</button></div>)}
  </div>;
}

function Kept({ draftKey }: { draftKey: string }) {
  const snap = useMachine(profileSaveFor(draftKey));
  return <>{snap.value === 'naming' && snap.context.name ? `Your new name “${snap.context.name}” is safe and sound.` : 'No worries, your unsaved edits are still here.'}</>;
}
