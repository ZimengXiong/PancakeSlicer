import { useEffect, useState } from 'react';
import { Check } from './Check';
import { InfoTip } from './InfoTip';
import './agent-preferences.css';

const RELOAD: Record<string, string> = {
  'claude-desktop': 'Quit Claude completely, then reopen it.',
  'claude-code': 'Start a new Claude Code session.',
  cursor: 'Restart Cursor.',
  codex: 'Start a new Codex session.',
};

const errorMessage = (error: unknown) => error instanceof Error ? error.message : String(error);

export function AgentPreferences() {
  const bridge = window.orcaDesktop?.agents;
  const [state, setAgents] = useState<DesktopAgents | null>(null);
  const [error, setError] = useState('');
  const [feedback, setFeedback] = useState('');
  const [pendingClient, setPendingClient] = useState<string | null>(null);
  const [selectedClient, setSelectedClient] = useState('');
  const [pendingAccess, setPendingAccess] = useState<Pick<DesktopAgents, 'enabled' | 'printers'> | null>(null);
  useEffect(() => { bridge?.get().then(setAgents, (error) => setError(errorMessage(error))); }, [bridge]);
  if (!bridge) return null;
  const access = pendingAccess ?? state;
  const configuredClients = state?.clients.filter((client) => client.configured) ?? [];
  const addableClients = state?.clients.filter((client) => client.available && !client.configured) ?? [];
  const updateAccess = async (enabled: boolean, printers: boolean) => {
    if (pendingAccess) return;
    setPendingAccess({ enabled, printers });
    setError('');
    try {
      const saved = await bridge.set(enabled, printers);
      setAgents((current) => current && { ...current, ...saved });
    } catch (e) {
      setError(errorMessage(e));
    } finally {
      setPendingAccess(null);
    }
  };
  const updateClient = async (id: string, name: string, add: boolean) => {
    setPendingClient(id);
    setFeedback('');
    try {
      const { clients } = await bridge.client(id, add);
      setAgents((current) => current && { ...current, clients });
      setSelectedClient('');
      setError('');
      setFeedback(add ? `${name} configured. ${RELOAD[id] ?? ''}`.trim() : `${name} configuration removed.`);
    } catch (e) {
      setError(errorMessage(e));
    } finally {
      setPendingClient(null);
    }
  };
  const addSelectedClient = () => {
    const client = addableClients.find((candidate) => candidate.id === selectedClient);
    if (client) void updateClient(client.id, client.name, true);
  };
  const copyConfig = async (format: 'codex' | 'json') => {
    setFeedback('');
    try {
      await bridge.copyConfig(format);
      setError('');
      setFeedback(format === 'codex' ? 'TOML copied.' : 'JSON copied.');
    } catch (e) {
      setError(errorMessage(e));
    }
  };
  return <>
    <h3 className="modal-group">AI agents</h3>
    <label className="modal-check"><Check checked={!!access?.enabled} disabled={!state || !!pendingAccess} onCheckedChange={(next) => void updateAccess(next, next && !!state?.printers)} />
      <span>Let AI agents use PancakeSlicer<InfoTip label="AI agents">{'Agents such as Claude or Cursor connect through PancakeSlicer\'s MCP server while the app is open: they can read your profiles, slice model files and check the G-code.\nTurn this off to cut them off at once.'}</InfoTip></span></label>
    {state && access?.enabled && <>
      <label className="modal-check"><Check checked={access.printers} disabled={!!pendingAccess} onCheckedChange={(next) => void updateAccess(true, next)} />
        <span>Let agents send prints and control printers<InfoTip label="Printer access">{'Lets agents use the camera and light, send or upload jobs, and pause, resume or stop prints on your connected printers.\nAgents are told to ask you before every print.'}</InfoTip></span></label>
      <table className="agent-client-table" aria-label="MCP clients">
        <thead><tr><th scope="col">Client</th><th scope="col">Status</th><th scope="col">Action</th></tr></thead>
        <tbody>
          {configuredClients.map((client) => <tr key={client.id}>
            <th scope="row">{client.name}</th>
            <td>{client.current ? 'Configured' : 'Update available'}</td>
            <td>
              {!client.current && <button type="button" className="btn" aria-label={`Update ${client.name}`}
                disabled={pendingClient !== null} onClick={() => void updateClient(client.id, client.name, true)}>Update</button>}{' '}
              <button type="button" className="btn" aria-label={`Remove ${client.name}`}
                disabled={pendingClient !== null} onClick={() => void updateClient(client.id, client.name, false)}>Remove</button>
            </td>
          </tr>)}
          <tr>
            <td colSpan={2}><select className="text" aria-label="Configuration to add" value={selectedClient}
              disabled={pendingClient !== null} onChange={(e) => setSelectedClient(e.target.value)}>
              <option value="">Select a configuration…</option>
              {addableClients.map((client) =>
                <option key={client.id} value={client.id}>{client.name}</option>)}
            </select></td>
            <td><button type="button" className="btn" disabled={!selectedClient || pendingClient !== null}
              onClick={addSelectedClient}>{pendingClient ? 'Working…' : 'Add'}</button></td>
          </tr>
        </tbody>
      </table>
      <div className="agent-config-copy">
        <button type="button" className="btn" onClick={() => void copyConfig('codex')}>Copy TOML</button>
        <button type="button" className="btn" onClick={() => void copyConfig('json')}>Copy JSON</button>
      </div>
      {feedback && <div className="hint-text" role="status">{feedback}</div>}
    </>}
    {error && <p className="hint-text" role="alert">{error}</p>}
  </>;
}
