import { project } from '../flow/project';
import { plateSignature } from '../slice-input';
import { getState, setState, subscribeStore } from '../state/store';
import { SAMPLE_JOB } from './config';

export async function showSampleSlice() {
  const { project: id } = await (await fetch('/api/demo/sample')).json() as { project: string };
  const status = await (await fetch(`/api/jobs/${SAMPLE_JOB}`)).json();
  let opened = false;
  const check = () => {
    const s = getState();
    if (project.get().context.id !== id || !s.objects.length || s.resolved.state !== 'ready' || s.job.id) return;
    setState({ job: { id: SAMPLE_JOB, state: 'done', pct: 100, stage: 'done', status, plateSignature: plateSignature(s) }, ...(opened ? {} : { tab: 'preview' as const }) });
    opened = true;
  };
  subscribeStore(check); check();
}
