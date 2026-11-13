import { useMemo, useRef, useState } from 'react';
import { confirmDialog } from '../components/Ask';
import { Dropdown } from '../components/Dropdown';
import { getState, resolvePreset, toast, useStore } from '../state/store';
import {
  adoptConnection, connectionOf, discoverPrinters, KIND_LABEL, removeConnection, saveConnection, statusLine, statusTone, tempLine, testConnection, unassigned,
  type ConnectionEdit, type ConnectionKind, type ConnectionTest, type Discovered,
} from './connections';
import { printerModel } from './capabilities';
import './printers.css';

const sentence = (s: string) => s.charAt(0).toUpperCase() + s.slice(1);
const errorText = (e: unknown) => sentence(e instanceof Error ? e.message : String(e));
const STAGE: Record<string, string> = {
  unreachable: 'Unreachable', serial: 'Serial number doesn\'t match', access_code: 'Access code doesn\'t match',
  developer_mode: 'Developer Mode is turned off', address: 'Address isn\'t allowed', invalid: 'Double-check the address',
};
const DESKTOP = typeof window !== 'undefined' && !!(window as { orcaDesktop?: unknown }).orcaDesktop;

function defaultKind(printer: string): ConnectionKind {
  const { base, config } = resolvePreset(getState(), 'machine', printer);
  if (base?.vendor === 'BBL' || /bambu/i.test(base?.name ?? '')) return 'bambu-lan';
  if (base?.vendor === 'Prusa') return 'prusalink';
  if (config?.host_type === 'elegoolink' || base?.vendor === 'Elegoo') return 'elegoolink';
  return String(config?.gcode_flavor ?? '') === 'klipper' ? 'moonraker' : 'octoprint';
}

export function ConnectionEditor({ printer, name, onSaved }: { printer: string; name: string; onSaved?(): void }) {
  const saved = useStore((s) => connectionOf(s.printerConnections, printer));
  const status = useStore((s) => s.printerConnections.status[printer]);
  const all = useStore((s) => s.printerConnections);
  const earlier = useMemo(() => unassigned(all), [all]);
  const machines = useStore((s) => s.machines);
  const [kind, setKind] = useState<ConnectionKind>(saved?.kind ?? defaultKind(printer));
  const [host, setHost] = useState(saved?.host ?? '');
  const [serial, setSerial] = useState(saved?.serial ?? '');
  const [url, setUrl] = useState(saved?.url?.replace(/^http:\/\//, '') ?? '');
  const [auth, setAuth] = useState(saved?.auth === 'digest' ? 'digest' : 'apikey');
  const [user, setUser] = useState(saved?.username ?? '');
  const [secret, setSecret] = useState('');
  const [model, setModel] = useState(saved?.model ?? '');
  const [test, setTest] = useState<ConnectionTest | null>(null);
  const [found, setFound] = useState<{ printers: Discovered[]; notes: string[] } | null>(null);
  const [working, setWorking] = useState('');
  const [error, setError] = useState('');
  const sameKind = saved?.kind === kind;
  const bambu = kind === 'bambu-lan', http = !bambu && kind !== 'bambu-connect';
  const digest = kind === 'prusalink' && auth === 'digest';
  const setupModel = useStore((s) => String(resolvePreset(s, 'machine', printer).config?.printer_model ?? '')) || undefined;
  const keyOptional = kind === 'moonraker' || kind === 'duet' || kind === 'elegoolink';
  const edit = (): ConnectionEdit => ({ kind, name, model: model || (kind === 'elegoolink' ? setupModel : undefined),
    ...(bambu ? { host: host.trim(), serial: serial.trim(), access_code: secret } : {}),
    ...(http ? { url: url.trim(), key: secret, auth: kind === 'prusalink' ? auth : undefined, username: digest ? user.trim() : undefined } : {}) });
  const hasSecret = !!saved?.has_secret && sameKind;
  const ready = kind === 'bambu-connect' || (bambu ? !!host.trim() && !!serial.trim() && (!!secret || hasSecret)
    : !!url.trim() && (keyOptional || !!secret || hasSecret) && (!digest || !!user.trim()));
  const fields = useRef(0);
  const touched = () => { fields.current++; setTest(null); };
  const changed = <T,>(set: (v: T) => void) => (v: T) => { set(v); touched(); };

  const run = async (what: string, action: () => Promise<void>) => {
    setWorking(what); setError('');
    try { await action(); } catch (e) { setError(errorText(e)); } finally { setWorking(''); }
  };
  const modelOf = (preset?: string) => preset ? machines.find((m) => m.name === preset)?.model : undefined;
  const use = async (d: Discovered) => {
    const found = modelOf(d.preset);
    if (found && setupModel && found !== setupModel && !await confirmDialog('Printer model mismatch',
      `Heads up! ${d.model ?? found} at ${d.host ?? d.url} doesn't match your ${setupModel} profile. Sending ${name} G-code to a different printer model could crash the toolhead or ruin the print. Send it anyway?`,
      ['Use anyway', 'Cancel'], true)) return;
    setKind(d.kind); touched();
    if (d.kind === 'bambu-lan') { setHost(d.host ?? ''); setSerial(d.serial ?? ''); setModel(d.model ?? ''); }
    else setUrl((d.url ?? '').replace(/^http:\/\//, ''));
  };
  const addAsNew = (d: Discovered) => run('add', async () => {
    const { createProfileFromBase } = await import('../profiles/catalog');
    const key = await createProfileFromBase('machine', d.preset!, d.name || `My ${d.model ?? KIND_LABEL[d.kind]}`);
    const label = resolvePreset(getState(), 'machine', key).user?.name ?? d.name ?? key;
    if (d.kind !== 'bambu-lan') await saveConnection(key, { kind: d.kind, name: label, url: d.url });
    toast(d.kind === 'bambu-lan' ? `Added ${label}! Pop into its Connection settings to enter the access code.` : `Added ${label} and connected it`);
  });

  return (
    <div className="connection-editor">
      {saved && (
        <div className="send-target" role="status">
          <span className="status-text" data-tone={statusTone(status)}>{statusLine(status) || 'Saved'}</span>
          {tempLine(status, printerModel(getState(), printer) || model) && <span className="send-meta">{tempLine(status, printerModel(getState(), printer) || model)}</span>}
        </div>
      )}
      <label className="form-row"><span>Host type</span>
        <Dropdown className="combo" value={kind} onValueChange={(v) => changed(setKind)(v as ConnectionKind)}
          options={(Object.keys(KIND_LABEL) as ConnectionKind[]).map((k) => ({ value: k, label: KIND_LABEL[k] }))} />
      </label>
      {bambu && <>
        <label className="form-row"><span>Printer IP</span><input className="text" value={host} placeholder="192.168.1.60" spellCheck={false} onChange={(e) => changed(setHost)(e.target.value)} /></label>
        <label className="form-row"><span>Serial number</span><input className="text" value={serial} spellCheck={false} onChange={(e) => changed(setSerial)(e.target.value)} /></label>
        <label className="form-row"><span>Access code</span><input className="text" type="password" autoComplete="new-password" value={secret} placeholder={hasSecret ? 'Stored' : ''} onChange={(e) => changed(setSecret)(e.target.value)} /></label>
        <div className="hint-text connection-hint">On your printer screen, go to Settings › Network › LAN Only and turn on Developer Mode to find your access code.</div>
      </>}
      {kind === 'bambu-connect' && <div className="hint-text">{DESKTOP
        ? "Sending opens your sliced file in Bambu Connect on this computer to print through your Bambu account—handy when your printer is in cloud mode."
        : "Bambu Connect runs locally on your computer, so direct sending requires the desktop app. In a browser, just download the .gcode.3mf file and open it in Bambu Connect."}</div>}
      {http && <>
        <label className="form-row"><span>Hostname, IP or URL</span><input className="text" value={url} placeholder={kind === 'moonraker' ? '192.168.1.70:7125' : '192.168.1.70'} spellCheck={false} onChange={(e) => changed(setUrl)(e.target.value)} /></label>
        {kind === 'prusalink' && (
          <label className="form-row"><span>Authorization</span>
            <Dropdown className="combo" value={auth} onValueChange={(v) => changed(setAuth)(v)} options={[{ value: 'apikey', label: 'API key' }, { value: 'digest', label: 'HTTP digest' }]} />
          </label>
        )}
        {digest && <label className="form-row"><span>User</span><input className="text" value={user} autoComplete="off" spellCheck={false} onChange={(e) => changed(setUser)(e.target.value)} /></label>}
        <label className="form-row"><span>{kind === 'elegoolink' ? 'Access code / API key' : digest || kind === 'duet' ? 'Password' : 'API key'}{keyOptional ? ' (optional)' : ''}</span>
          <input className="text" type="password" autoComplete="new-password" value={secret} placeholder={hasSecret ? 'Stored' : ''} onChange={(e) => changed(setSecret)(e.target.value)} />
        </label>
        {kind === 'elegoolink' && <div className="hint-text connection-hint">Centauri Carbon uses only the IP address. Centauri 2 uses its LAN access code (default 123456).</div>}
      </>}
      {working === 'test' && <div role="status" className="hint-text">Testing…</div>}
      {test && (test.ok ? <div role="status" className="send-ok">{sentence(test.message)}</div>
        : <div role="alert" className="warning-text">{STAGE[test.stage] ? `${STAGE[test.stage]}: ` : ''}{test.message}</div>)}
      {error && <div role="alert" className="warning-text">{error}</div>}
      <div className="connection-buttons">
        <button type="button" className="btn" disabled={!ready || !!working} onClick={() => void run('test', async () => {
          const mine = fields.current;
          const result = await testConnection(printer, edit());
          if (mine !== fields.current) return;
          setTest(result);
          if (result.ok && result.model && (bambu || kind === 'elegoolink')) setModel(result.model);
        })}>Test connection</button>
        <button type="button" className="btn-primary" disabled={!ready || !!working} onClick={() => void run('save', async () => {
          await saveConnection(printer, edit()); setSecret(''); toast(`Saved connection for ${name}!`); onSaved?.();
        })}>Save</button>
        <button type="button" className="btn" disabled={!!working} onClick={() => void run('find', async () => setFound(await discoverPrinters()))}>{working === 'find' ? 'Searching…' : 'Find printers'}</button>
        {saved && <button type="button" className="btn-ghost connection-remove" disabled={!!working} onClick={() => void run('remove', async () => { await removeConnection(printer); toast(`Removed connection for ${name}`); })}>Remove connection</button>}
      </div>
      {found && (!found.printers.length
        ? <div className="hint-text">Hmm, we didn't find any printers on your network.{found.notes.length ? ` ${found.notes.join(' ')}` : ''}</div>
        : <table className="found-table" aria-label="Printers found"><tbody>
          {found.printers.map((d) => {
            const usedBy = d.assigned && d.assigned !== printer ? resolvePreset(getState(), 'machine', d.assigned).user?.name ?? 'another printer' : '';
            const preset = d.preset && machines.some((m) => m.name === d.preset);
            const other = modelOf(d.preset), mismatch = !!other && !!setupModel && other !== setupModel;
            const label = d.model ?? d.name ?? KIND_LABEL[d.kind];
            return (
              <tr key={`${d.kind}${d.serial ?? ''}${d.host ?? ''}${d.url ?? ''}`}>
                <th scope="row">{label}</th>
                <td className="found-addr">{d.host ?? d.url?.replace(/^http:\/\//, '')}</td>
                <td className="found-kind">{KIND_LABEL[d.kind]}</td>
                <td className="found-tags">
                  {mismatch && <span className="found-tag" aria-label={`Not a ${setupModel}`}>other model</span>}
                  {usedBy && <span className="found-tag" aria-label={`Used by ${usedBy}`}>in use</span>}
                  {d.connection === 'cloud' && <span className="found-tag">cloud mode</span>}
                </td>
                <td className="found-acts">
                  <button type="button" className="btn-ghost" disabled={!!working} aria-label={`Use ${label} for ${name}`} onClick={() => void use(d)}>Use</button>
                  {preset && <button type="button" className="btn-ghost" disabled={!!working} aria-label={`Add ${label} as a new printer`} onClick={() => void addAsNew(d)}>Add</button>}
                </td>
              </tr>
            );
          })}
        </tbody></table>)}
      {!saved && earlier.length > 0 && (
        <table className="found-table" aria-label="Saved printer connections">
          <caption>Saved connections</caption>
          <tbody>{earlier.map((c) => (
            <tr key={c.id}>
              <th scope="row">{c.model ?? c.name ?? KIND_LABEL[c.kind]}</th>
              <td className="found-addr">{c.host ?? c.url?.replace(/^http:\/\//, '')}</td>
              <td className="found-kind">{KIND_LABEL[c.kind]}</td>
              <td className="found-tags" />
              <td className="found-acts">
                <button type="button" className="btn-ghost" disabled={!!working} aria-label={`Use this connection for ${name}`} onClick={() => void run('adopt', async () => { await adoptConnection(printer, c.id, name); onSaved?.(); })}>Use</button>
                <button type="button" className="btn-ghost" disabled={!!working} aria-label="Remove this connection" onClick={() => void run('remove', () => removeConnection(`unassigned:${c.id}`))}>Remove</button>
              </td>
            </tr>
          ))}</tbody>
        </table>
      )}
    </div>
  );
}
