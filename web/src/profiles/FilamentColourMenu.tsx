import { setupOf } from '../printers/device-slots';
import { useEffect, useId, useRef, useState } from 'react';
import { Popover } from 'radix-ui';
import { getState, useStore } from '../state/store';
import { inkOn } from '../components/SlotChip';
import { FILAMENT_PALETTE } from '../components/filament-palette';
import { colourName, EMPTY_SLOT_COLOUR, reportedTray, slotColour } from './filament-colour';
import { useMachine } from '../flow/fsm';
import { slotColourFor } from '../flow/slot-colour';
import { slotFilaments } from './slots';
import { InLayer } from '../components/PageLayer';

const PALETTE = FILAMENT_PALETTE.map((c) => c.hex);
const normal = (v: string) => /^#?[\da-f]{6}$/i.test(v) ? `#${v.replace('#', '').toUpperCase()}` : null;

export function FilamentColourMenu({ slot, printer }: { slot: number; printer?: string }) {
  const errorId = useId();
  const key = useStore((s) => printer ?? s.printer);
  useStore((s) => s.records); useStore((s) => slotFilaments(s));
  useStore((s) => JSON.stringify(reportedTray(s, slot, key) ?? null));
  const s = getState(), colour = slotColour(s, slot, key), reported = reportedTray(s, slot, key);
  const stored = setupOf(s, key)?.slots[slot];
  const empty = colour === EMPTY_SLOT_COLOUR || (!reported && !stored?.colour && !(key === s.printer ? slotFilaments(s)[slot] : stored?.filament));
  const [open, setOpen] = useState(false), [draft, setDraft] = useState(colour);
  const m = slotColourFor(key, slot), snap = useMachine(m), busy = snap.value === 'writing', error = snap.value === 'failed' ? snap.context.error : null;
  const was = useRef(snap.value);
  useEffect(() => { if (was.current === 'writing' && snap.value === 'idle') setOpen(false); was.current = snap.value; }, [snap.value]);
  const picked = normal(draft), same = picked === normal(colour) && !empty;
  const toPrinter = !!reported && s.printerConnections.connections.some((c) => c.printer === key && c.kind === 'bambu-lan');
  const apply = () => m.send({ type: 'APPLY', colour: picked! });
  const name = empty ? 'empty' : colourName(colour);
  return <Popover.Root open={open} onOpenChange={(next) => { if (busy) return; setOpen(next); if (next) { setDraft(empty ? PALETTE[0] : colour); if (m.get().value === 'failed') m.send({ type: 'DISMISS' }); } }}>
    <Popover.Trigger asChild>{empty
      ? <button type="button" className="color-trigger is-empty" aria-label={`Slot ${slot + 1} colour: empty`}>{slot + 1}</button>
      : <button type="button" className="color-trigger" style={{ backgroundColor: colour }} data-ink={inkOn(colour)} aria-label={`Slot ${slot + 1} colour: ${name}${reported ? ', from the printer' : ''}`}>{slot + 1}</button>}
    </Popover.Trigger>
    <InLayer portal={Popover.Portal}><Popover.Content className="orca-menu color-popover" aria-label={`Slot ${slot + 1} colour`} side="bottom" align="start" sideOffset={4} collisionPadding={12} onKeyDown={(e) => e.stopPropagation()}>
      <div className="color-heading"><span>{picked ? colourName(picked) : 'Colour'}</span><span className="color-preview" style={{ backgroundColor: picked ?? colour }}/></div>
      <div className="color-palette" role="group" aria-label="Palette">{PALETTE.map((c) => <button key={c} type="button" className="color-swatch" style={{ backgroundColor: c }} aria-label={colourName(c)} aria-pressed={picked === c} disabled={busy} onClick={() => setDraft(c)}/>)}</div>
      <label className="color-hex"><span>HEX</span><input className="text" aria-label="HEX colour" aria-invalid={!picked} aria-describedby={!picked ? errorId : undefined} value={draft} spellCheck={false} maxLength={7} disabled={busy}
        onChange={(e) => setDraft(e.target.value)} onKeyDown={(e) => { if (e.key === 'Enter' && picked && !same && !busy) apply(); }}/></label>
      {!picked && <div className="control-error" id={errorId} role="status">Please enter a 6-digit HEX color (like #FF6B00).</div>}
      {toPrinter && <p className="color-note">Also updates this AMS tray on your printer!</p>}
      {error && <p className="control-error" role="alert">{error}</p>}
      <div className="color-actions"><button type="button" className="btn primary" disabled={!picked || same || busy} aria-busy={busy || undefined} onClick={apply}>{busy ? (toPrinter ? 'Saving to the printer…' : 'Saving…') : 'Apply'}</button></div>
    </Popover.Content></InLayer>
  </Popover.Root>;
}
