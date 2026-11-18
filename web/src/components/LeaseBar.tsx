import { useEffect } from 'react';
import { takeControl, useProjectLease } from '../flow/project-lease';
import { Sep } from '../separator';
const EDIT_KEYS = /^(Delete|Backspace)$/;
const EDIT_CHORDS = /^[xvzydai]$/i;

export default function LeaseBar() {
  const { value, context } = useProjectLease();
  const readOnly = value === 'viewing' || value === 'lost';
  const busy = value === 'requesting' && context.take;
  const show = readOnly || busy;
  useEffect(() => {
    const root = document.documentElement;
    if (!show) { delete root.dataset.readonly; return; }
    root.dataset.readonly = 'true';
    const hold = (e: KeyboardEvent) => {
      if (e.target instanceof Element && e.target.closest('input,textarea,select,[contenteditable="true"],dialog')) return;
      if (EDIT_KEYS.test(e.key) || ((e.ctrlKey || e.metaKey) && EDIT_CHORDS.test(e.key))) { e.preventDefault(); e.stopPropagation(); }
    };
    const block = (e: Event) => { if (!(e.target instanceof Element && e.target.closest('dialog'))) { e.preventDefault(); e.stopPropagation(); } };
    window.addEventListener('keydown', hold, true);
    window.addEventListener('paste', block, true);
    window.addEventListener('drop', block, true);
    return () => { delete root.dataset.readonly; window.removeEventListener('keydown', hold, true); window.removeEventListener('paste', block, true); window.removeEventListener('drop', block, true); };
  }, [show]);
  if (!show) return null;
  return (
    <div className="app-notices"><div className="lease-bar" role="status">
      <span>Currently open in another tab or device{context.holder && <><Sep/>{context.holder.label}</>}</span>
      <button type="button" className="btn-primary" disabled={busy} aria-busy={busy} onClick={takeControl}>{busy ? 'Taking control…' : 'Take control'}</button>
    </div></div>
  );
}
