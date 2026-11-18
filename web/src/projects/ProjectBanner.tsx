import { useRef } from 'react';
import { saveProject } from '../actions';
import { useMachine } from '../flow/fsm';
import { keepProjectVersion, project, reloadProject, restoreAndSave, retryProject, saveAsNewProject, showProjects, useProject } from '../flow/project';
import { useProjectLease } from '../flow/project-lease';
import { bootRecovered } from '../boot-shape';
import { DRAFT_CONFLICT_MESSAGE, RECOVERED_MESSAGE } from '../flow/project-recovery';
import { inNoticeLane, useNoticeLane, useTokenAsked } from '../components/Notices';

export function HeldProjectBanner() {
  const opening = useMachine(project, (p) => p.value === 'blank' || p.value === 'opening');
  const recovered = opening && bootRecovered();
  const lane = useNoticeLane(recovered);
  if (!recovered) return null;
  const conflict = recovered === 'conflict';
  return inNoticeLane(<div role="alert" className="toast toast-error" aria-busy="true">
    <div className="toast-text"><div>{conflict ? DRAFT_CONFLICT_MESSAGE : RECOVERED_MESSAGE}</div></div>
    {(conflict ? ['Reload latest', 'Keep mine'] : ['Retry', 'Reload saved']).map((label) => <button key={label} type="button" className="btn" disabled>{label}</button>)}
  </div>, lane);
}

export function ProjectBanner() {
  const { value, context: now } = useProject();
  const lease = useProjectLease().value;
  const held = useRef<{ context: typeof now; conflict: boolean } | null>(null);
  if (value === 'failed' || value === 'conflict') held.current = { context: now, conflict: value === 'conflict' };
  else if (value !== 'saving') held.current = null;
  const token = useTokenAsked();
  const shown = lease !== 'viewing' && lease !== 'lost' && !token ? held.current : null;
  const lane = useNoticeLane(!!shown);
  if (!shown) return null;
  const { context } = shown, conflict = shown.conflict || /another tab/.test(context.error ?? '');
  return inNoticeLane(
    <div role="alert" className="toast toast-error">
      <div className="toast-text"><div>{conflict && context.failure?.name === 'ProjectConflictError' ? 'Changed in another window' : context.error ?? 'Not saved'}</div></div>
      {context.failure?.kind === 'deleted' && !context.loadError ? <>
        <button type="button" className="btn" onClick={() => void restoreAndSave()}>Restore &amp; save</button>
        <button type="button" className="btn" onClick={() => void saveAsNewProject()}>Save as new</button>
      </> : context.loadError ? <>
        <button type="button" className="btn" onClick={() => void retryProject()}>Retry</button>
        <button type="button" className="btn" onClick={showProjects}>Projects</button>
      </> : conflict ? <>
        <button type="button" className="btn" onClick={() => void reloadProject()}>Reload latest</button>
        <button type="button" className="btn" onClick={() => void keepProjectVersion()}>Keep mine</button>
      </> : <>
        <button type="button" className="btn" onClick={() => void retryProject()}>Retry</button>
        {context.failure?.kind === 'interrupted'
          ? <button type="button" className="btn" onClick={() => void reloadProject()}>Reload saved</button>
          : <button type="button" className="btn" data-output onClick={() => void saveProject()}>Export</button>}
      </>}
    </div>, lane);
}
