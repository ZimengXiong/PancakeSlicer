import { DropdownMenu as Menu } from 'radix-ui';
import { filamentColor } from '../plater/filamentColor';
import { EMPTY_SLOT_COLOUR, reportedTray } from '../profiles/filament-colour';
import { profileInfo } from '../profiles/view';
import { getState, useStore, type State } from '../state/store';
import './menu.css';
import { slotFilaments } from '../profiles/slots';
import { InLayer } from './PageLayer';

export const inkOn = (hex: string) => {
  const m = /^#?([\da-f]{2})([\da-f]{2})([\da-f]{2})/i.exec(hex);
  return m && 0.2126 * parseInt(m[1], 16) + 0.7152 * parseInt(m[2], 16) + 0.0722 * parseInt(m[3], 16) > 150 ? 'dark' : 'light';
};

export function SlotChip({ n, colour }: { n: number; colour: string }) {
  if (colour === EMPTY_SLOT_COLOUR) return <span className="slot-chip is-empty" aria-hidden>{n}</span>;
  return <span className="slot-chip" aria-hidden data-ink={inkOn(colour)} style={{ backgroundColor: colour }}>{n}</span>;
}

export const slotsOf = (s: State) => slotFilaments(s).map((id, i) => ({ n: i + 1, colour: filamentColor(s, i + 1), name: profileInfo(s, 'filament', id).name || 'Unknown filament' }));
export function useSlots() {
  useStore((s) => s.records);
  useStore((s) => JSON.stringify(slotFilaments(s).map((_, i) => reportedTray(s, i) ?? null)));
  return slotsOf(getState());
}

export function SlotPicker({ value, onChange, label, disabled = false, inherit, painted = [] }: {
  value: number;
  onChange(slot: number): void;
  label: string;
  disabled?: boolean;
  inherit?: number;
  painted?: number[];
}) {
  const slots = useSlots();
  const shown = value || inherit || 1, colour = slots[shown - 1]?.colour ?? '#808080';
  const paints = painted.filter((n) => n !== shown);
  const parts = [shown, ...paints];
  const chip = paints.length ? <span className="slot-split" aria-hidden>{parts.map((n) => { const c = slots[n - 1]?.colour ?? '#808080';
    return <span key={n} data-ink={inkOn(c)} style={{ backgroundColor: c }}>{n}</span>; })}</span>
    : <SlotChip n={shown} colour={colour} />;
  const named = `${label}: slot ${shown}${paints.length ? `, painted ${paints.join(', ')}` : ''}`;
  if (slots.length < 2 && !inherit) return <span className="slot-pick" role="img" aria-label={named}>{chip}</span>;
  return (
    <Menu.Root>
      <Menu.Trigger asChild disabled={disabled}>
        <button type="button" className="slot-pick" data-follows={value === 0 && inherit ? '' : undefined} aria-label={`${named}${value === 0 && inherit ? ' (the object’s)' : ''}`}
          onClick={(e) => e.stopPropagation()} onPointerDown={(e) => e.stopPropagation()}>{chip}</button>
      </Menu.Trigger>
      <InLayer portal={Menu.Portal}>
        <Menu.Content className="orca-menu slot-menu" data-menu-family="orca" align="end" sideOffset={4} collisionPadding={8} onClick={(e) => e.stopPropagation()}>
          {inherit !== undefined && (
            <Menu.Item className="menu-item" data-current={value === 0 ? '' : undefined} onSelect={() => onChange(0)}>
              <SlotChip n={inherit} colour={slots[inherit - 1]?.colour ?? '#808080'} /><span className="orca-menu-label">Object’s filament</span>
            </Menu.Item>
          )}
          {slots.map((slot) => (
            <Menu.Item key={slot.n} className="menu-item" data-current={slot.n === value ? '' : undefined} onSelect={() => onChange(slot.n)}>
              <SlotChip n={slot.n} colour={slot.colour} /><span className="orca-menu-label">{slot.name}</span>
            </Menu.Item>
          ))}
        </Menu.Content>
      </InLayer>
    </Menu.Root>
  );
}
