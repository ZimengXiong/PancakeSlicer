import { recordProblems } from '../persist/profile-library';
import { useStore } from '../state/store';

export function ProblemLine({ text, className = '', role }: { text?: string; className?: string; role?: string }) {
  return text ? <span role={role} className={`record-problem ${className}`} data-problem data-truncated="true" data-full-text={text}>{text}</span> : null;
}

export function RecordProblem({ kind, id, className }: { kind: 'machine' | 'process' | 'filament'; id: string; className?: string }) {
  const text = useStore((s) => {
    const list = kind === 'machine' ? s.records.printers : kind === 'process' ? s.records.processes : s.records.filaments;
    const name = (list as { id: string; name: string }[]).find((r) => r.id === id)?.name ?? '';
    return recordProblems(s, kind, id, name).join('; ');
  });
  return <ProblemLine text={text || undefined} className={className}/>;
}
