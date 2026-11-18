import { applyHealth, type SurfaceArtifactManifest } from '../api/client';
import { readHealth } from '../api/health';
import { assertNever, createMachine, useMachine } from './fsm';

export type EngineState = 'unknown' | 'healthy' | 'degraded' | 'qualified' | 'unqualified';
export interface EngineHealth {
  artifact_manifest?: { features?: Record<string, unknown>; binary_sha256?: string; resources_sha256?: string;
    validated?: boolean; preview_ready?: boolean; print_qualified?: boolean };
  engine_state?: string;
}
type HealthEvent = { type: 'HEALTH'; health: EngineHealth } | { type: 'FAIL' };
interface HealthContext { health: EngineHealth | null }
export function classifyEngine(health: EngineHealth): Exclude<EngineState, 'unknown'> {
  const manifest = health.artifact_manifest;
  if (health.engine_state === 'degraded' || health.engine_state === 'down') return 'degraded';
  if (!manifest) return 'healthy';
  const hashes = /^[a-f0-9]{64}$/i.test(manifest.binary_sha256 ?? '') && /^[a-f0-9]{64}$/i.test(manifest.resources_sha256 ?? '');
  return hashes && manifest.print_qualified === true ? 'qualified' : 'unqualified';
}
const healthTransitions = (['healthy', 'degraded', 'qualified', 'unqualified'] as const).map((target) => ({
  target, guard: (_: HealthContext, e: Extract<HealthEvent, { type: 'HEALTH' }>) => classifyEngine(e.health) === target,
  action: (_: HealthContext, e: Extract<HealthEvent, { type: 'HEALTH' }>) => ({ health: e.health }),
}));
const on = { HEALTH: healthTransitions, FAIL: 'degraded' } as const;
export const engineHealth = createMachine<EngineState, HealthEvent, HealthContext>({
  id: 'engine-health', initial: 'unknown', strict: true, context: { health: null },
  states: { unknown: { on }, healthy: { on }, degraded: { on }, qualified: { on }, unqualified: { on } },
});
let probe: Promise<void> | null = null, retry: ReturnType<typeof setTimeout> | undefined, watching = false;
function watch() {
  if (watching) return;
  watching = true;
  document.addEventListener('visibilitychange', () => { if (document.visibilityState === 'visible') void probeEngine(); });
  window.addEventListener('online', () => void probeEngine());
}
export function probeEngine(): Promise<void> {
  if (probe) return probe;
  watch();
  clearTimeout(retry);
  probe = readHealth(5000).then(async (r) => {
    if (!r.ok) throw new Error('Service status check unavailable');
    const health = await r.json() as EngineHealth;
    applyHealth(health as { artifact_manifest?: SurfaceArtifactManifest });
    engineHealth.send({ type: 'HEALTH', health });
  }).catch(() => engineHealth.send({ type: 'FAIL' })).finally(() => {
    probe = null;
    if (engineHealth.get().value === 'degraded') retry = setTimeout(() => void probeEngine(), 10000);
  });
  return probe;
}
export function enginePermission(): 'unknown' | 'preview' | 'print' | 'unavailable' {
  const state = engineHealth.get().value;
  switch (state) {
    case 'unknown': return 'unknown';
    case 'healthy': case 'unqualified': return 'preview';
    case 'qualified': return 'print';
    case 'degraded': return 'unavailable';
    default: return assertNever(state);
  }
}
export const useEngineHealth = () => useMachine(engineHealth);
