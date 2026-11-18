import { MenuButton, type MenuEntry } from '../components/Menu';
import { Caret } from '../components/Caret';

export interface SaveTargets { profile?: () => void; forPrinter?: { model: string; run: () => void }; asNew: () => void }

export function SaveMenu({ name, targets, busy = false, disabled = false }: { name: string; targets: SaveTargets; busy?: boolean; disabled?: boolean }) {
  const items: MenuEntry[] = [
    ...(targets.profile ? [{ label: `To ${name}`, action: targets.profile }] : []),
    ...(targets.forPrinter ? [{ label: `To ${name} for ${targets.forPrinter.model}`, action: targets.forPrinter.run }] : []),
    { label: 'As new…', action: targets.asNew },
  ];
  return <MenuButton className="btn primary pf-save-menu" label="Save" disabled={disabled} aria-busy={busy || undefined} items={items}>
    {busy && <span className="pf-spin" aria-hidden/>}{busy ? 'Saving…' : 'Save'}<Caret dir="down"/>
  </MenuButton>;
}
