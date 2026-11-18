import { useSyncExternalStore } from 'react';
import { noticeText, type NoticeCode } from '../notice-codes';
import { retryServiceSession, serviceSessionSnapshot, serviceSessionUnreachable, subscribeServiceSession } from '../persist/service-session';
import { inNoticeLane, useNoticeLane, useTokenAsked } from './Notices';
export default function SessionNotice() {
  const session = useSyncExternalStore(subscribeServiceSession, serviceSessionSnapshot);
  const token = useTokenAsked(), failed = (session.state === 'failed' || session.state === 'conflict') && !token;
  const lane = useNoticeLane(failed);
  if (!failed) return null;
  const code: NoticeCode = session.state === 'conflict' ? 'session_conflict'
    : session.code === 'access_token_needed' || session.code === 'server_storage_full' ? session.code
    : serviceSessionUnreachable() ? 'session_retrying' : 'session_refused';
  return inNoticeLane(<div className="toast toast-error" role="alert"><div className="toast-text"><div>{noticeText(code, undefined)}</div></div><button type="button" className="btn" onClick={() => void retryServiceSession().catch(() => {})}>Retry</button></div>, lane);
}
