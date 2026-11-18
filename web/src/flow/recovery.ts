import { loadServiceSession, patchServiceSession, sessionDocument } from '../persist/service-session';
import { requireWorkspaceService } from '../persist/service';
export interface Recovery {
  version: 1;
  job?: { id: string; stage: string; seq: number; signature: string; projectId: string };
  send?: { jobId: string; target: string };
}
const identifier = (s: unknown): s is string => typeof s === 'string' && /^[A-Za-z0-9._-]{1,160}$/.test(s);
export async function initialiseRecovery(): Promise<void> { await requireWorkspaceService(); await loadServiceSession(); }
export function readRecovery(): Recovery {
  try {
    const r = sessionDocument().fsmRecovery as Recovery | undefined;
    if (r?.version !== 1) return { version: 1 };
    return { version: 1,
      ...(identifier(r.job?.id) && identifier(r.job?.projectId) && typeof r.job.signature === 'string' && r.job.signature.length <= 32
        && typeof r.job.stage === 'string' && r.job.stage.length < 80 && Number.isSafeInteger(r.job.seq) && r.job.seq >= -1 ? { job: r.job } : {}),
      ...(identifier(r.send?.jobId) && typeof r.send.target === 'string' && r.send.target.length <= 160 ? { send: r.send } : {}) };
  } catch { return { version: 1 }; }
}
export function rememberRecovery(patch: Partial<Omit<Recovery, 'version'>>) {
  patchServiceSession({ fsmRecovery: { ...readRecovery(), ...patch, version: 1 } });
}

export function recoverySignature(signature: string): string {
  let a = 2166136261, b = 5381;
  for (let i = 0; i < signature.length; i++) { a = Math.imul(a ^ signature.charCodeAt(i), 16777619); b = Math.imul(b, 33) ^ signature.charCodeAt(i); }
  return `${(a >>> 0).toString(16)}-${(b >>> 0).toString(16)}`;
}
