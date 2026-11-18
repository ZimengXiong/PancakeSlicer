import { preferenceJson, savePreferences, usePreference } from '../saved-preferences';
import { useId, useState } from 'react';
import { Popover } from 'radix-ui';
import { Range } from './Range';
import { FILAMENT_PALETTE, paletteName } from './filament-palette';
import './controls.css';
import { InLayer } from './PageLayer';
const KEY = 'orca.web.recent-colors';
const PALETTE = FILAMENT_PALETTE.map((c) => c.hex);
const normalize = (s: string) => /^#?[\da-f]{6}$/i.test(s) ? `#${s.replace('#', '').toUpperCase()}` : null;
const read = (): string[] => { const x = preferenceJson<unknown>(KEY, []); return Array.isArray(x) ? x.map(String).map(normalize).filter((v): v is string => !!v).slice(0,8) : []; };
export default function ColorControl({value, onCommit, label = 'Color', disabled, ...named}: {value: string; onCommit(value: string): void; label?: string; disabled?: boolean; 'aria-labelledby'?: string}) {
  const id = useId();
  const [open, setOpen] = useState(false), [draft, setDraft] = useState(value);
  usePreference(KEY); const recent = read();
  const valid = normalize(draft), color = valid ?? normalize(value) ?? '#000000';
  const rgb = [1,3,5].map(i => parseInt(color.slice(i,i+2),16));
  const setChannel = (i: number, n: number) => setDraft('#' + rgb.map((v,k) => (k === i ? Math.max(0,Math.min(255,Math.round(n))) : v).toString(16).padStart(2,'0')).join('').toUpperCase());
  const apply = () => {
    if (!valid) return;
    onCommit(valid);
    const next = [valid, ...recent.filter(c => c !== valid)].slice(0,8);
    savePreferences({[KEY]:next});
    setOpen(false);
  };
  const swatches = (colors: string[], group: string) => <div className="color-palette" role="group" aria-label={group}>{colors.map(c => <button key={c} type="button" className="color-swatch" style={{backgroundColor: c}} aria-label={paletteName(c) ?? c} aria-pressed={color === c} onClick={() => setDraft(c)} />)}</div>;
  return <Popover.Root open={open} onOpenChange={next => { setOpen(next); if (next) setDraft(normalize(value) ?? '#000000'); }}>
    <Popover.Trigger asChild><button {...named} type="button" className="color-trigger" style={{backgroundColor:normalize(value) ?? undefined}} data-unknown={!normalize(value)} aria-label={`${label}: ${normalize(value) ?? 'Not set'}`} disabled={disabled}/></Popover.Trigger>
    <InLayer portal={Popover.Portal}><Popover.Content className="orca-menu color-popover" aria-label={label} side="bottom" align="start" sideOffset={6} collisionPadding={12} onKeyDown={e => e.stopPropagation()}>
      <form onSubmit={e => {e.preventDefault(); apply();}}>
        <div className="color-heading"><span>{label}</span><span className="color-preview" style={{backgroundColor: color}}/></div>
        {swatches(PALETTE, 'Palette')}
        {recent.length > 0 && <><div className="color-caption">Recent</div>{swatches(recent, 'Recent colors')}</>}
        <label className="color-hex"><span>HEX</span><input className="text" aria-label="HEX color" aria-invalid={!valid} aria-describedby={!valid ? `${id}-error` : undefined} value={draft} spellCheck={false} maxLength={7} onChange={e => setDraft(e.target.value)}/></label>
        {!valid && <div className="control-error" id={`${id}-error`} role="status">Please enter a 6-digit HEX color (like #FF6B00).</div>}
        <div className="color-channels">{['R','G','B'].map((key,i) => <div key={key} className="color-channel"><label htmlFor={`${id}-${key}`}>{key}</label><Range min={0} max={255} value={rgb[i]} onValueChange={n => setChannel(i,n)} aria-label={`${key} channel`}/><input id={`${id}-${key}`} className="text num" aria-label={`${key} value`} inputMode="numeric" type="number" min={0} max={255} value={rgb[i]} onChange={e => { if(e.target.value !== '') setChannel(i, Number(e.target.value)); }}/></div>)}</div>
        <div className="color-actions"><Popover.Close asChild><button type="button" className="btn-ghost">Cancel</button></Popover.Close><button type="submit" className="btn" disabled={!valid}>Apply</button></div>
      </form>
    </Popover.Content></InLayer>
  </Popover.Root>;
}
