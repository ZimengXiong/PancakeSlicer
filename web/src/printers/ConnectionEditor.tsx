import { useMemo, useState } from 'react';
import { useMachine } from '../flow/fsm';
import { connectionEditFor, type ConnectionEditContext, type ConnectionOp } from '../flow/connection-edit';
import { confirmDialog } from '../components/Ask';
import { Dropdown } from '../components/Dropdown';
import { getState, toast, useStore, type State } from '../state/store';
import { recordOf, selectionValues, useAskedValues, type ProfileValues } from '../profiles/view';
import { loadedPrinterVendors, printerCatalog, type CatalogModel } from '../profiles/catalog-reads';
import type { PrinterRecord } from '../profiles/records';
import {
  adoptConnection, connectionOf, discoverPrinters, KIND_LABEL, removeConnection, saveConnection, statusLine, statusTone, tempLine, testConnection, unassigned,
  type ConnectionEdit, type ConnectionKind, type Discovered,
} from './connections';
import { printerModel } from './capabilities';
import './printers.css';
import { addPrinterOnVariant } from '../profiles/catalog-add';

const sentence = (s: string) => s.charAt(0).toUpperCase() + s.slice(1);
const errorText = (e: unknown) => sentence(e instanceof Error ? e.message : String(e));
const STAGE: Record<string, string> = {
  unreachable: 'Unreachable', serial: 'Serial number doesn\'t match', access_code: 'Access code doesn\'t match',
  developer_mode: 'Developer Mode is turned off', address: 'Address isn\'t allowed', invalid: 'Double-check the address',
};
const DESKTOP = typeof window !== 'undefined' && !!(window as { orcaDesktop?: unknown }).orcaDesktop;

const printerOf = (s: State, id: string) => recordOf(s, 'machine', id) as PrinterRecord | undefined;
function defaultKind(vendor: string | undefined, model: string | undefined, values: ProfileValues | null): ConnectionKind | null {
  if (vendor === 'BBL') return 'bambu-lan';
  if (vendor === 'Prusa') return 'prusalink';
  if (vendor === 'Flashforge') return 'flashforge';
  if (model?.startsWith('Elegoo Centauri')) return 'elegoolink';
  if (values?.state !== 'ready') return null;
  return String(values.values.gcode_flavor ?? '') === 'klipper' ? 'moonraker' : 'octoprint';
}
function foundModel(d: Discovered): CatalogModel | undefined {
  if (!d.preset || !d.catalogModel) return undefined;
  for (const v of loadedPrinterVendors() ?? []) for (const model of v.models) if (model.id === d.catalogModel) return model;
  return undefined;
}

export function ConnectionEditor({ printer, name, onSaved }: { printer: string; name: string; onSaved?(): void }) {
  const saved = useStore((s) => connectionOf(s.printerConnections, printer));
  const status = useStore((s) => s.printerConnections.status[printer]);
  const all = useStore((s) => s.printerConnections);
  const earlier = useMemo(() => unassigned(all), [all]);
  const vendor = useStore((s) => printerOf(s, printer)?.vendor), setupModel = useStore((s) => printerOf(s, printer)?.model);
  const selected = useStore((s) => s.printer === printer ? selectionValues(s, 'machine') : null);
  const asked = useAskedValues(!saved && !selected && vendor !== 'BBL' && vendor !== 'Prusa' ? { type: 'machine', id: printer } : null);
  const [picked, setKind] = useState<ConnectionKind | null>(saved?.kind ?? null);
  const kindValues = selected ?? asked, kind = picked ?? defaultKind(vendor, setupModel, kindValues);
  const [host, setHost] = useState(saved?.host ?? '');
  const [serial, setSerial] = useState(saved?.serial ?? '');
  const [url, setUrl] = useState(saved?.url?.replace(/^http:\/\//, '') ?? '');
  const [slug, setSlug] = useState(saved?.slug ?? '');
  const [auth, setAuth] = useState(saved?.auth === 'digest' ? 'digest' : 'apikey');
  const [user, setUser] = useState(saved?.username ?? '');
  const [secret, setSecret] = useState('');
  const [model, setModel] = useState(saved?.model ?? '');
  const m = connectionEditFor(printer), snap = useMachine(m), { test, found, error } = snap.context, working = snap.value !== 'idle';
  const sameKind = saved?.kind === kind;
  const bambu = kind === 'bambu-lan', flashforge = kind === 'flashforge', repetier = kind === 'repetier', http = !!kind && !bambu && !flashforge && kind !== 'bambu-connect';
  const digest = kind === 'prusalink' && auth === 'digest';
  const keyOptional = kind === 'moonraker' || kind === 'duet' || kind === 'elegoolink';
  const edit = (): ConnectionEdit => ({ kind: kind!, name, model: kind === 'elegoolink' ? setupModel : model || undefined,
    ...(bambu || flashforge ? { host: host.trim(), serial: serial.trim(), access_code: secret } : {}),
    ...(http ? { url: url.trim(), slug: repetier ? slug.trim() : undefined, key: secret, auth: kind === 'prusalink' ? auth : undefined, username: digest ? user.trim() : undefined } : {}) });
  const hasSecret = !!saved?.has_secret && sameKind;
  const ready = !!kind && (kind === 'bambu-connect' || (flashforge ? !!host.trim() && (!secret || !!serial.trim()) : bambu ? !!host.trim() && !!serial.trim() && (!!secret || hasSecret)
    : !!url.trim() && (!repetier || !!slug.trim()) && (keyOptional || !!secret || hasSecret) && (!digest || !!user.trim())));
  const touched = () => m.send({ type: 'EDIT' });
  const changed = <T,>(set: (v: T) => void) => (v: T) => { set(v); touched(); };

  const run = (op: ConnectionOp, action: () => Promise<Partial<ConnectionEditContext> | void>) =>
    m.send({ type: 'RUN', op, run: () => action().catch((e: unknown) => { throw new Error(errorText(e)); }) });
  const modelOf = (d: Discovered) => foundModel(d)?.id;
  const use = async (d: Discovered) => {
    const found = modelOf(d);
    if (found && setupModel && found !== setupModel && !await confirmDialog('Printer model mismatch',
      `Heads up! ${d.model ?? found} at ${d.host ?? d.url} doesn't match your ${setupModel} profile. Sending ${name} G-code to a different printer model could crash the toolhead or ruin the print. Send it anyway?`,
      ['Use anyway', 'Cancel'], true)) return;
    setKind(d.kind); touched();
    if (d.kind === 'bambu-lan' || d.kind === 'flashforge') { setHost(d.host ?? ''); setSerial(d.serial ?? ''); setModel(d.model ?? ''); }
    else setUrl((d.url ?? '').replace(/^http:\/\//, ''));
  };
  const addAsNew = (d: Discovered) => run('add', async () => {
        const key = await addPrinterOnVariant(foundModel(d)!, d.preset!, d.name || undefined);
    const label = printerOf(getState(), key)?.name ?? key;
    if (d.kind !== 'bambu-lan') await saveConnection(key, { kind: d.kind, name: label, url: d.url });
    (d.kind === 'bambu-lan' ? toast('added_pop_into_connection', { label }) : toast('added_connected', { label }));
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
        <Dropdown className="combo" value={kind ?? ''} placeholder={kindValues?.state === 'failed' ? kindValues.error ?? 'Hmm, we couldn\'t find that printer' : 'Resolving…'} onValueChange={(v) => changed(setKind)(v as ConnectionKind)}
          options={(Object.keys(KIND_LABEL) as ConnectionKind[]).map((k) => ({ value: k, label: KIND_LABEL[k] }))} />
      </label>
      {bambu && <>
        <label className="form-row"><span>Printer IP</span><input className="text" value={host} placeholder="192.168.1.60" spellCheck={false} onChange={(e) => changed(setHost)(e.target.value)} /></label>
        <label className="form-row"><span>Serial number</span><input className="text" value={serial} spellCheck={false} onChange={(e) => changed(setSerial)(e.target.value)} /></label>
        <label className="form-row"><span>Access code</span><input className="text" type="password" autoComplete="new-password" value={secret} placeholder={hasSecret ? 'Stored' : ''} onChange={(e) => changed(setSecret)(e.target.value)} /></label>
        <div className="hint-text connection-hint">On your printer screen, go to Settings › Network › LAN Only and turn on Developer Mode to find your access code.</div>
      </>}
      {flashforge && <>
        <label className="form-row"><span>Printer IP</span><input className="text" value={host} placeholder="192.168.1.60" spellCheck={false} onChange={(e) => changed(setHost)(e.target.value)} /></label>
        <label className="form-row"><span>Serial number (optional)</span><input className="text" value={serial} spellCheck={false} onChange={(e) => changed(setSerial)(e.target.value)} /></label>
        <label className="form-row"><span>Check code (optional)</span><input className="text" type="password" autoComplete="new-password" value={secret} placeholder={hasSecret ? 'Stored' : ''} onChange={(e) => changed(setSecret)(e.target.value)} /></label>
        <div className="hint-text connection-hint">Just the IP address is enough to get started! If you also add the serial number and check code from your printer's network settings, uploads can use its faster HTTP API.</div>
      </>}
      {kind === 'bambu-connect' && <div className="hint-text">{DESKTOP
        ? "Sending opens your sliced file in Bambu Connect on this computer to print through your Bambu account—handy when your printer is in cloud mode."
        : "Bambu Connect runs locally on your computer, so direct sending requires the desktop app. In a browser, just download the .gcode.3mf file and open it in Bambu Connect."}</div>}
      {http && <>
        <label className="form-row"><span>Hostname, IP or URL</span><input className="text" value={url} placeholder={kind === 'moonraker' ? '192.168.1.70:7125' : '192.168.1.70'} spellCheck={false} onChange={(e) => changed(setUrl)(e.target.value)} /></label>
        {repetier && <>
          <label className="form-row"><span>Printer slug</span><input className="text" value={slug} placeholder="my_printer" spellCheck={false} onChange={(e) => changed(setSlug)(e.target.value)} /></label>
          <div className="hint-text connection-hint">Use the slug from printer/list data[].slug, not a TCP port. Upload and autostart are supported; live status, pause, and temperature controls are unavailable.</div>
        </>}
        {kind === 'prusalink' && (
          <label className="form-row"><span>Authorization</span>
            <Dropdown className="combo" value={auth} onValueChange={(v) => changed(setAuth)(v)} options={[{ value: 'apikey', label: 'API key' }, { value: 'digest', label: 'HTTP digest' }]} />
          </label>
        )}
        {digest && <label className="form-row"><span>User</span><input className="text" value={user} autoComplete="off" spellCheck={false} onChange={(e) => changed(setUser)(e.target.value)} /></label>}
        <label className="form-row"><span>{kind === 'elegoolink' ? 'Access code' : digest || kind === 'duet' ? 'Password' : 'API key'}{keyOptional ? ' (optional)' : ''}</span>
          <input className="text" type="password" autoComplete="new-password" value={secret} placeholder={hasSecret ? 'Stored' : ''} onChange={(e) => changed(setSecret)(e.target.value)} />
        </label>
        {kind === 'elegoolink' && <div className="hint-text connection-hint">Only Centauri 2 uses an access code (default 123456).</div>}
      </>}
      {snap.value === 'testing' && <div role="status" className="hint-text">Testing…</div>}
      {test && (test.ok ? <div role="status" className="send-ok">{sentence(test.message)}</div>
        : <div role="alert" className="warning-text">{STAGE[test.stage] ? `${STAGE[test.stage]}: ` : ''}{test.message}</div>)}
      {error && <div role="alert" className="warning-text">{error}</div>}
      <div className="connection-buttons">
        <button type="button" className="btn" disabled={!ready || working} onClick={() => run('test', async () => {
          const result = await testConnection(printer, edit());
          if (result.ok && result.model && bambu) setModel(result.model);
          return { test: result };
        })}>Test connection</button>
        <button type="button" className="btn-primary" disabled={!ready || working} onClick={() => run('save', async () => {
          await saveConnection(printer, edit()); setSecret(''); toast('saved_connection', { name }); onSaved?.();
        })}>Save</button>
        <button type="button" className="btn" disabled={working} onClick={() => run('find', async () => ({ found: (await Promise.all([discoverPrinters(), printerCatalog()]))[0] }))}>{snap.value === 'finding' ? 'Searching…' : 'Find printers'}</button>
        {saved && <button type="button" className="btn-ghost connection-remove" disabled={working} onClick={() => run('remove', async () => { await removeConnection(printer); toast('removed_connection', { name }); })}>Remove connection</button>}
      </div>
      {found && (!found.printers.length
        ? <div className="hint-text">Hmm, we didn't find any printers on your network.{found.notes.length ? ` ${found.notes.join(' ')}` : ''}</div>
        : <table className="found-table" aria-label="Printers found"><tbody>
          {found.printers.map((d) => {
            const usedBy = d.assigned && d.assigned !== printer ? printerOf(getState(), d.assigned)?.name ?? 'another printer' : '';
            const preset = !!foundModel(d);
            const other = modelOf(d), mismatch = !!other && !!setupModel && other !== setupModel;
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
                  <button type="button" className="btn-ghost" disabled={working} aria-label={`Use ${label} for ${name}`} onClick={() => void use(d)}>Use</button>
                  {preset && <button type="button" className="btn-ghost" disabled={working} aria-label={`Add ${label} as a new printer`} onClick={() => void addAsNew(d)}>Add</button>}
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
                <button type="button" className="btn-ghost" disabled={working} aria-label={`Use this connection for ${name}`} onClick={() => run('adopt', async () => { await adoptConnection(printer, c.id, name); onSaved?.(); })}>Use</button>
                <button type="button" className="btn-ghost" disabled={working} aria-label="Remove this connection" onClick={() => run('remove', async () => { await removeConnection(`unassigned:${c.id}`); })}>Remove</button>
              </td>
            </tr>
          ))}</tbody>
        </table>
      )}
    </div>
  );
}
