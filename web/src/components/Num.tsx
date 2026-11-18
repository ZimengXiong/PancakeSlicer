import { useEffect, useId, useState } from 'react';
import { toast } from '../state/store';

export interface NumIssue { inline: string; detail: string }

export function Num({ value, onCommit, step = 1, label, axis, check }: { value?: number; onCommit: (v: number) => void; step?: number; label?: string; axis?: 'X' | 'Y' | 'Z'; check?: (v: number) => NumIssue | undefined }) {
  const shown = value === undefined ? '' : value.toFixed(2);
  const [text, setText] = useState(shown);
  const errorId = useId();
  useEffect(() => setText(shown), [shown]);
  const typed = text.trim() !== '' && Number.isFinite(Number(text)) ? Number(text) : undefined;
  const issue = typed !== undefined && text !== shown ? check?.(typed) : undefined;
  const commit = () => {
    if (typed === undefined) return setText(shown);
    if (issue) {
      toast('kept_instead_2', { value: label ?? 'Value', value2: text.trim(), detail: issue.detail, shown });
      return setText(shown);
    }
    onCommit(typed);
  };
  const input = (
    <input
      className="text num manip-num"
      aria-label={label}
      inputMode="decimal"
      disabled={value === undefined}
      placeholder={value === undefined ? '—' : undefined}
      value={text}
      step={step}
      aria-invalid={!!issue || undefined}
      aria-describedby={issue ? errorId : undefined}
      onChange={(e) => setText(e.target.value)}
      onBlur={commit}
      onKeyDown={(e) => e.key === 'Enter' && (e.target as HTMLInputElement).blur()}
    />
  );
  const field = axis ? <span className="manip-axis"><span className={`axis ${axis.toLowerCase()}`} aria-hidden>{axis}</span>{input}</span> : input;
  return issue ? <span className="manip-cell">{field}<span className="control-error" id={errorId} role="status">{issue.inline}</span></span> : field;
}
