import type { ReactNode } from 'react';
import { toast } from '../state/store';
import { copyToClipboard } from '../clipboard';

export function copyText(text: string) {
  (copyToClipboard(text) ? toast('copied', { text }) : toast('browser_blocked_clipboard_access'));
}

export function Copyable({ value, children }: { value: string; children?: ReactNode }) {
  const copy = () => { if (!window.getSelection()?.toString()) copyText(value); };
  return (
    <button type="button" className="copyable" title="Copy" onClick={copy}>{children ?? value}</button>
  );
}
