import { useEffect, useSyncExternalStore } from 'react';
import * as THREE from 'three';
import { getState, setState, useStore, type PlateObject } from '../state/store';
import { instanceBox, instanceMatrix } from '../plater/geometry';
import { meshStats, subscribeMeshStats, meshStatsVersion, type MeshStats as Stats } from '../plater/meshStats';
import { Copyable } from './Copyable';

function objectStats(o: PlateObject): Stats {
  const det = (m: THREE.Matrix4) => Math.abs(m.determinant());
  const own = meshStats(o.geometry), scale = det(instanceMatrix(o, o.instances[0]));
  const total = { volume: own.volume * scale, triangles: own.triangles, openEdges: own.openEdges };
  for (const v of o.volumes ?? []) {
    if (v.type !== 'part') continue;
    const vs = meshStats(v.geometry);
    total.volume += vs.volume * scale * det(new THREE.Matrix4().fromArray(v.matrix));
    total.triangles += vs.triangles;
    total.openEdges += vs.openEdges;
  }
  return total;
}

const mm = (x: number) => x.toFixed(2);
const volume = (mm3: number) => (mm3 >= 1000 ? `${(mm3 / 1000).toFixed(2)} cm³` : `${mm3.toFixed(1)} mm³`);
const idle = (f: () => void) => (typeof requestIdleCallback === 'function' ? requestIdleCallback(f) : setTimeout(f, 50));

export default function ObjectInfo() {
  const statsVersion = useSyncExternalStore(subscribeMeshStats, meshStatsVersion);
  const objects = useStore((s) => s.objects);
  const sel = useStore((s) => s.selected);
  const gizmo = useStore((s) => s.gizmo);
  useEffect(() => {
    let stop = false;
    const queue = objects.filter((o) => !o.project?.placeholder && !o.native);
    const next = () => {
      const o = queue.shift();
      if (stop || !o) return;
      const k = objectStats(o).openEdges, prev = getState().meshErrors;
      if ((prev[o.id] ?? 0) !== k) setState({ meshErrors: { ...prev, [o.id]: k } });
      idle(next);
    };
    idle(next);
    return () => { stop = true; };
  }, [objects, statsVersion]);
  const o = sel && !sel.all && sel.volume === undefined && (gizmo === 'none' || gizmo === 'move' || gizmo === 'rotate' || gizmo === 'scale')
    ? objects.find((ob) => ob.id === sel.object) : undefined;
  if (!o || o.project?.placeholder || o.native) return null;
  const st = objectStats(o), inst = o.instances.find((i) => i.id === sel!.instance) ?? o.instances[0];
  const size = instanceBox(o, inst).getSize(new THREE.Vector3());
  return (
    <div className="object-info" role="status" aria-label="Object info">
      <span><Copyable value={`${mm(size.x)} × ${mm(size.y)} × ${mm(size.z)} mm`} /></span>
      <span><Copyable value={volume(st.volume)} /></span>
      <span className="object-info-tris"><Copyable value={`${st.triangles.toLocaleString('en-US')} triangles`} /></span>
      {o.sourceUrl && <a className="notif-link" href={o.sourceUrl} target="_blank" rel="noopener noreferrer" title={o.sourceUrl}>Source URL</a>}
      {st.openEdges > 0 && <span className="object-info-error">
        {st.openEdges} non-manifold edge{st.openEdges === 1 ? '' : 's'}
        {<button type="button" className="notif-link" onClick={() => void import('../plater/meshOps').then((m) => m.fixModel(o.id))}>Fix Model</button>}
      </span>}
    </div>
  );
}
