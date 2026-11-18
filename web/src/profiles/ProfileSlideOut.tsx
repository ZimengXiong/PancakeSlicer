import { recordOf } from './view';
import { useEffect, useMemo, useRef, useState } from 'react';
import { closeSlideOut, openSlideOutEditor } from '../navigation';
import { getState, useStore, type SlideOutTarget } from '../state/store';
import { ProfileEditor } from './ProfileEditor';
import { projectDeltaKey, sharedDraft, updateSharedDraft } from './shared-draft';
import { focusSetting } from './settings-focus';
import './profile-slide-out.css';

const reduced = () => matchMedia('(prefers-reduced-motion: reduce)').matches;

export default function ProfileSlideOut() {
  const target = useStore((s) => (s.tab === 'prepare' ? s.slideOut : null));
  const [shown, setShown] = useState<SlideOutTarget | null>(target), [closing, setClosing] = useState(false), panel = useRef<HTMLElement>(null);
  useEffect(() => { if (target) panel.current?.focus({ preventScroll: true }); }, [target?.at]); // eslint-disable-line react-hooks/exhaustive-deps
  useEffect(() => {
    if (target) { setShown(target); setClosing(false); return; }
    if (!shown) return;
    setClosing(true);
    const t = setTimeout(() => { setShown(null); setClosing(false); }, reduced() ? 0 : 150);
    return () => clearTimeout(t);
  }, [target]); // eslint-disable-line react-hooks/exhaustive-deps
  useEffect(() => {
    if (!target) return;
    const key = (e: KeyboardEvent) => {
      if (e.key !== 'Escape' || e.defaultPrevented || (e.target as Element | null)?.closest?.('[role=menu], [role=listbox], [role=dialog], .modal')) return;
      e.preventDefault();
      const field = e.target instanceof HTMLInputElement || e.target instanceof HTMLTextAreaElement ? e.target : null;
      if (field && panel.current?.contains(field)) { field.blur(); return; }
      closeSlideOut();
    };
    window.addEventListener('keydown', key, true);
    return () => window.removeEventListener('keydown', key, true);
  }, [!!target]); // eslint-disable-line react-hooks/exhaustive-deps
  useEffect(() => {
    if (!target?.setting) return;
    const t = setTimeout(() => focusSetting(target.type, target.setting!), reduced() ? 0 : 220);
    return () => clearTimeout(t);
  }, [target?.at]); // eslint-disable-line react-hooks/exhaustive-deps
  if (!shown) return null;
  return <aside ref={panel} tabIndex={-1} className="profile-slide-out" data-state={closing ? 'closing' : 'open'} aria-label="Profile settings">
    <div className="pso-content"><SlideOutEditor key={`${shown.type}:${shown.key}`} target={shown}/></div>
  </aside>;
}

function SlideOutEditor({ target: { type, key } }: { target: SlideOutTarget }) {
  const drafts = useStore((s) => s.libraryDrafts), records = useStore((s) => s.records);
  const deltaKey = useStore((s) => projectDeltaKey(s, type, key));
  const answer = useStore((s) => s.resolved.answer);
  const draft = useMemo(() => sharedDraft(getState(), type, key, deltaKey, drafts[JSON.stringify([type, key])]), [type, key, deltaKey, drafts, records, answer]);
  const source = recordOf(getState(), type, key);
  if (!source) return <div className="pso-missing" role="status">Looks like this profile isn't in your library anymore.</div>;
  return <ProfileEditor type={type} name={key} draft={draft} update={updateSharedDraft} onBack={closeSlideOut} onClose={closeSlideOut} onSavedAs={(created) => openSlideOutEditor(type, created)} foldActions
    onRemove={() => void import('./LibraryPage').then((m) => m.removePreset(type, key))}/>;
}
