import { useSyncExternalStore } from 'react';
import { jobRequest } from './actions';
import { getState, type State } from './state/store';

type Answer = number | null | undefined;
const answers = new Map<string, Answer>();
const listeners = new Set<() => void>();
const emit = () => listeners.forEach((l) => l());
const subscribe = (l: () => void) => (listeners.add(l), () => void listeners.delete(l));

interface PlanRegion { object: number; curved: boolean; curved_share: number; planar_limit: string | null; area_xy?: number; area?: number }

async function plan(key: string, s: State, objectId: number) {
  const o = s.objects.find((p) => p.id === objectId);
  if (!o || !s.api || s.project) return void (answers.set(key, null), emit());
  const smoothest: State = {
    ...s,
    dirty: { ...s.dirty, process: { ...s.dirty.process, nonplanar_mode: 'nonplanar_top' } },
    objects: s.objects.map((p) => (p.id === objectId && p.settings.nonplanar_mode !== undefined ? { ...p, settings: { ...p.settings, nonplanar_mode: 'nonplanar_top' } } : p)),
  };
  try {
    const id = await s.api.submit({ ...(await jobRequest(smoothest)), events: [], output: 'gcode', plan_tops: true });
    const status = await new Promise<{ record?: unknown }>((resolve, reject) => {
      const stop = s.api!.events(id, { progress() {}, done(st) { stop(); resolve(st); }, error(m) { stop(); reject(new Error(m)); } });
    });
    const record = status.record as { schema?: string; regions?: PlanRegion[] } | undefined;
    const index = s.objects.indexOf(o);
    const regions = record?.schema === 'orca.nonplanar.plan_tops.v1' ? (record.regions ?? []).filter((r) => r.object === index && r.planar_limit !== 'flat') : [];
    const area = (r: PlanRegion) => r.area_xy ?? r.area ?? 0;
    const total = regions.reduce((a, r) => a + area(r), 0);
    answers.set(key, regions.some((r) => r.curved) && total > 0 ? regions.reduce((a, r) => a + r.curved_share * area(r), 0) / total : null);
  } catch {
    answers.set(key, null);
  }
  emit();
}

export function smoothestShare(objectId: number | undefined): Answer {
  const s = getState();
  if (objectId == null || !s.job.id) return null;
  const key = `${s.job.id}:${objectId}`;
  if (!answers.has(key)) {
    answers.set(key, undefined);
    void plan(key, s, objectId);
  }
  return answers.get(key);
}

export const useSmoothestShare = (objectId: number | undefined) => useSyncExternalStore(subscribe, () => smoothestShare(objectId));
