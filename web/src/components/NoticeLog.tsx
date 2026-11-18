import { useStore } from '../state/store';
import './notices.css';

export function NoticeLog() {
  const entries = useStore((s) => s.noticeLog);
  if (!entries.length) return null;
  return (
    <details className="notice-log" onKeyDown={(e) => {
      if (e.key !== 'Escape') return;
      e.stopPropagation();
      e.currentTarget.open = false;
      e.currentTarget.querySelector('summary')?.focus();
    }}>
      <summary>Log</summary>
      <ol aria-label="Notice log">{entries.map((entry) => <li key={entry.id}>{entry.message}</li>)}</ol>
    </details>
  );
}
