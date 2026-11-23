import { useEffect, useState } from 'react';
import { Check } from './Check';
import { InfoTip } from './InfoTip';
import { copyToClipboard } from '../clipboard';

const RELOAD: Record<string, string> = {
  'claude-desktop': 'Added, quit Claude from the tray and reopen',
  'claude-code': 'Added, start a new session',
  cursor: 'Added, restart Cursor',
};

export function AgentPreferences() {
  const bridge = window.orcaDesktop?.agents;
  const [state, setAgents] = useState<DesktopAgents | null>(null);
  const [error, setError] = useState('');
  const [copied, setCopied] = useState(false);
  useEffect(() => { bridge?.get().then(setAgents, (e) => setError(String(e?.message ?? e))); }, [bridge]);
  if (!bridge) return null;
  const run = (next: Promise<DesktopAgents>) => next.then((s) => { setAgents(s); setError(''); }, (e) => setError(String(e?.message ?? e)));
  const config = state && JSON.stringify({ mcpServers: { pancakeslicer: state.launch } }, null, 2);
  return <>
    <h3 className="modal-group">AI agents</h3>
    <label className="modal-check"><Check checked={!!state?.enabled} disabled={!state} onCheckedChange={(next) => void run(bridge.set(next, next && !!state?.printers))} />
      <span>Let AI agents use PancakeSlicer<InfoTip label="AI agents">{'Agents such as Claude or Cursor connect through PancakeSlicer\'s MCP server while the app is open: they can read your profiles, slice model files and check the G-code.\nTurn this off to cut them off at once.'}</InfoTip></span></label>
    <label className="modal-check"><Check checked={!!state?.printers} disabled={!state?.enabled} onCheckedChange={(next) => void run(bridge.set(true, next))} />
      <span>Let agents send prints and control printers<InfoTip label="Printer access">{'Lets agents use the camera and light, send or upload jobs, and pause, resume or stop prints on your connected printers.\nAgents are told to ask you before every print.'}</InfoTip></span></label>
    {state?.enabled && <>
      {state.clients.map((c) => <div key={c.id} className="form-row">
        <span>{c.name}</span>
        <span>{!c.available ? <span className="hint-text">Not installed</span>
          : c.current ? <button type="button" className="btn" onClick={() => void run(bridge.client(c.id, false))}>Remove</button>
          : <button type="button" className="btn" onClick={() => void run(bridge.client(c.id, true))}>{c.configured ? 'Update' : 'Add'}</button>}
          {c.current && <span className="hint-text"> {RELOAD[c.id] ?? `Added, restart ${c.name}`}</span>}</span>
      </div>)}
      <div className="form-row"><span>Other clients</span>
        <span><button type="button" className="btn" onClick={() => setCopied(!!config && copyToClipboard(config))}>Copy MCP config</button>{copied && <span className="hint-text"> Copied.</span>}</span></div>
    </>}
    {error && <p className="hint-text" role="alert">{error}</p>}
  </>;
}
