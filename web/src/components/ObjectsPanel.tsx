import { Suspense, useEffect, useState } from 'react';
import { useStore } from '../state/store';
import { narrowQuery } from '../workspace-sheet';
import { ObjectList } from './ObjectList';
import { preloadable } from '../preloadable';
import { Caret } from './Caret';
import { Skeleton } from './Skeleton';
const ObjectSettings = preloadable(() => import('./ObjectOverrides'));

const KEY = 'orca.objects-panel';
const saved = () => { try { return sessionStorage.getItem(KEY); } catch { return null; } };

export default function ObjectsPanel() {
  const selected = useStore((s) => !!s.selected);
  const count = useStore((s) => s.objects.length);
  const gizmo = useStore((s) => !!s.gizmo && s.gizmo !== 'none');
  const [open, setOpen] = useState(() => (saved() ?? (narrowQuery.matches ? '0' : '1')) === '1');
  const [peek, setPeek] = useState(false);
  useEffect(() => setPeek(false), [gizmo]);
  const shown = open && (!gizmo || peek);
  const toggle = () => {
    if (gizmo && open && !peek) return setPeek(true);
    try { sessionStorage.setItem(KEY, shown ? '0' : '1'); } catch {  }
    setOpen(!shown);
    setPeek(!shown);
  };
  return (
    <section className="objects-panel" data-open={shown || undefined} aria-label="Objects">
      <button type="button" className="op-head" aria-expanded={shown} onClick={toggle}>
        <span className="op-title">Objects</span><span className="op-count">{count}</span><Caret dir={shown ? 'up' : 'down'} />
      </button>
      {shown && <div className="op-body scroll-fade"><ObjectList /><Suspense fallback={selected ? <div className="ol-settings" role="status" aria-label="Loading settings"><div className="ol-settings-head"><Skeleton width={80} height={10}/></div>{[0, 1].map((i) => <div key={i} className="opt-line"><span className="opt-label"><Skeleton width="60%" height={10}/></span><span className="opt-field"><Skeleton block height={26} style={{ width: '100%' }}/></span></div>)}</div> : null}><ObjectSettings /></Suspense></div>}
    </section>
  );
}
