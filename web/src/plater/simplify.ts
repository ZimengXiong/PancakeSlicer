import { loadMesh } from './geometry';
import { getState, markStale, setState, toast } from '../state/store';
import { undoable } from '../state/history';
export type { SimplificationInfo } from './simplify-core';

const requests = new Map<number, number>();

export async function setKeepOriginal(id: number, keep: boolean): Promise<void> {
  const object = getState().objects.find((o) => o.id === id);
  if (!object?.simplification || object.project || object.native) return;
  if (object.paint && Object.values(object.paint).some(Boolean)) { toast('clear_painted_regions_first'); return; }
  const token = (requests.get(id) ?? 0) + 1;
  requests.set(id, token);
  const geometry = await loadMesh(object.source, undefined, !keep).catch((err: unknown) => { toast('simplify_failed', { detail: String(err) }); return null; });
  if (!geometry || requests.get(id) !== token) return;
  if (!getState().objects.some((o) => o.id === id && o.geometry === object.geometry)) return;
  const info = keep ? { ...object.simplification, kept: 'original' as const } : geometry.userData.simplification ?? object.simplification;
  undoable(keep ? 'Keep original' : 'Simplify', () => setState((s) => ({
    objects: s.objects.map((o) => o.id === id ? { ...o, geometry, simplification: info } : o),
    job: markStale(s.job),
  })));
}
