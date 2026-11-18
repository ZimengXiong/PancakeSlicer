import { useEffect, useState, useSyncExternalStore, type ReactNode } from 'react';
import { createPortal } from 'react-dom';
import { Glyph } from './Glyph';
import { slice } from '../actions';
import { sliceFailure } from '../notice-codes';
import { setState, useStore } from '../state/store';
import { InTopLayer, useModalCount } from './Modal';
import './notices.css';
import { copyToClipboard } from '../clipboard';
import { accessSnapshot, subscribeAccess } from '../api/access';


export function Toast() {
  const toast = useStore((s) => s.toast);
  const seq = useStore((s) => s.toastSeq);
  const action = useStore((s) => (s.toast && s.toastAction?.msg === s.toast ? s.toastAction : null));
  const ms = useStore((s) => s.toastMs);
  const kind = useStore((s) => s.toastKind) ?? 'info';
  const step = useStore((s) => s.toastStep);
  const msg = toast;
  const [held, setHeld] = useState(false);
  const overDialog = useModalCount() > 0;
  const waiting = overDialog && !!toast && kind !== 'error' && action?.label !== 'Undo';
  useEffect(() => {
    if (!toast || held || waiting) return;
    const t = setTimeout(() => setState((s) => (s.toast === toast ? { toast: null } : {})), ms ?? 3500);
    return () => clearTimeout(t);
  }, [toast, seq, held, waiting, ms]);
  useEffect(() => setHeld(false), [msg]);
  const lane = useNoticeLane(msg);
  const undoKey = overDialog && action?.label === 'Undo';
  useEffect(() => {
    if (!undoKey || !action) return;
    const onKey = (e: KeyboardEvent) => {
      if (!(e.ctrlKey || e.metaKey) || e.shiftKey || e.altKey || e.key.toLowerCase() !== 'z' || (e.target instanceof Element && e.target.closest('input,select,textarea,[contenteditable="true"]'))) return;
      e.preventDefault(); e.stopPropagation();
      setState({ toast: null }); action.run();
    };
    window.addEventListener('keydown', onKey, true);
    return () => window.removeEventListener('keydown', onKey, true);
  }, [undoKey, action]);
  if (!msg || waiting) return null;
  const card = (
      <div className={`toast toast-${kind}`} role={kind === 'error' || kind === 'warning' ? 'alert' : 'status'}
        onMouseEnter={() => setHeld(true)} onMouseLeave={() => setHeld(false)}
        onFocus={() => setHeld(true)} onBlur={() => setHeld(false)}>
        <div className="toast-text">
          <div>{msg}</div>
          {step && <div className="toast-step">{step}</div>}
        </div>
        {action && <button type="button" className="btn-ghost toast-action" aria-keyshortcuts={undoKey ? 'Control+Z Meta+Z' : undefined} onClick={() => { setState({ toast: null }); action.run(); }}>{action.label}</button>}
        {toast && (
          <button type="button" className="toast-close icon-btn" aria-label="Close" onClick={() => setState({ toast: null })}>
            <Glyph name="close" size={14} />
          </button>
        )}
      </div>
  );
  return inNoticeLane(card, lane);
}

export const useTokenAsked = () => useSyncExternalStore(subscribeAccess, accessSnapshot)?.status === 401;

export const inNoticeLane = (card: ReactNode, lane: Element | null) => lane ? createPortal(card, lane) : <InTopLayer>{card}</InTopLayer>;

export function useNoticeLane(msg: ReactNode): Element | null {
  const tab = useStore((s) => s.tab);
  const dialogOpen = useStore((s) => !!(s.dialog || s.appDialog || s.flushDialog || s.calibDialog || s.sessionDialog || s.presetUx));
  const loading = useStore((s) => !!s.loading);
  const [lane, setLane] = useState<Element | null>(null);
  useEffect(() => {
    setLane(msg && (tab === 'prepare' || tab === 'preview') && !dialogOpen && !loading ? document.querySelector('.canvas-area .notifications') : null);
  }, [msg, tab, dialogOpen, loading]);
  return lane;
}

export function SliceFailed({ error, code, onClose }: { error: string; code: string; onClose(): void }) {
  const { title, step } = sliceFailure(code);
  const canRetry = useStore((s) => s.objects.length > 0);
  const [copied, setCopied] = useState(false);
  return (
    <div className="notification failed slice-failed" role="alert">
      <button type="button" className="notif-close icon-btn" aria-label="Close" onClick={onClose}>
        <Glyph name="close" size={14} />
      </button>
      <div className="notif-title">{title}</div>
      <div className="notif-body">{step ?? error}</div>
      <div className="slice-failed-actions">
        {canRetry && <button type="button" className="btn-primary" onClick={() => void slice()}>Slice again</button>}
        <details>
          <summary>Details</summary>
          <pre className="slice-failed-raw">{error}</pre>
          <button type="button" className="notif-link" onClick={() => setCopied(copyToClipboard(error))}>
            {copied ? 'Copied' : 'Copy'}
          </button>
        </details>
      </div>
    </div>
  );
}

const LOADING_WORDS: Record<string, string> = { Connecting: 'Starting', 'Loading presets': 'Loading printers' };

export function LoadingState({ text }: { text: string }) {
  const m = /^(.*?)\s+(\d+)%$/.exec(text);
  const label = LOADING_WORDS[text] ?? (m ? m[1] : text);
  const pct = m ? Number(m[2]) : null;
  return (
    <div className="loading-state" role="status" aria-live="polite">
      <div className="loading-label"><span>{label}</span>{pct !== null && <span>{pct}%</span>}</div>
      <div className={`loading-bar ${pct === null ? 'indeterminate' : ''}`} role="progressbar" aria-label={label}
        aria-valuemin={0} aria-valuemax={100} aria-valuenow={pct ?? undefined}>
        <div style={pct === null ? undefined : { width: `${pct}%` }} />
      </div>
    </div>
  );
}
