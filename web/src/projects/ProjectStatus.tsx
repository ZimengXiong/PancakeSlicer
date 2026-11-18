import { useProject } from '../flow/project';
import './projects.css';

export function ProjectStatus() {
  return <LifecycleStatus />;
}

function LifecycleStatus() {
  const value = useProject((s) => s.value);
  if (value === 'opening') return <div className="pj-status" role="status"><span>Opening…</span></div>;
  return null;
}
