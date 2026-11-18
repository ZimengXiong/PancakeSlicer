import type { PresetType } from '../api/types';
import { modeVisible, OPTIONS, splitKey, TABS, type OptionMode } from '../options';
import { effectiveMode, getState, setState, toast, type State } from '../state/store';

const TAB_OF = { process: 'process', filament: 'filament', machine: 'printer' } as const;
const MODE_NAME: Partial<Record<OptionMode, string>> = { advanced: 'Advanced', develop: 'Expert', developer: 'Developer' };

export const locate = (type: PresetType, key: string) => {
  const pages = TABS[TAB_OF[type]]?.pages ?? [];
  for (const [pi, page] of pages.entries()) for (const [gi, group] of page.groups.entries())
    if (group.lines.some((l) => l.options.some((k) => splitKey(k)[0] === key))) return { pi, gi, page: page.title, group: group.title };
  return { pi: 1e9, gi: 0, page: 'Other', group: '' };
};

export function focusSetting(type: PresetType, key: string, name = OPTIONS[key]?.label || key) {
  const at = locate(type, key), rank = OPTIONS[key]?.mode, s = getState();
  if (at.pi === 1e9) return;
  const patch: Partial<State> = {};
  if (rank && !modeVisible(rank, effectiveMode(s))) {
    if (rank === 'developer') patch.developerMode = true; else patch.uiMode = rank;
    const prior = { uiMode: s.uiMode, developerMode: s.developerMode };
    toast('switched_reveal', { value: MODE_NAME[rank] ?? rank, name }, { action: { label: 'Undo', run: () => setState(prior) } });
  }
  const reveal = () => setState({ settingsFocus: { type, page: at.page, key, nonce: Date.now() } });
  if (Object.keys(patch).length) { setState(patch); setTimeout(reveal, 50); } else reveal();
}
