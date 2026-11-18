import { useRef, useState } from 'react';
import { openSlideOutEditor } from '../navigation';
import { useStore } from '../state/store';
import { profileInfo } from '../profiles/view';
import { AddPrintMenu } from '../profiles/AddPrintMenu';
import { requestPreset } from './PresetCombo';
import { listProfiles } from '../profiles/pickerSource';
import { currentEntry, UNAVAILABLE, PickerSkeleton, presets, sameStateGroups, usePickerPending } from '../profiles/Pickers';
import { Picker, type PickerGroup, type PickerItem } from './picker/Picker';
import { addPrintMaster, usePrintMasters } from '../profiles/print-catalog';
import './process-settings.css';
export function ProcessPresetPicker() {
  const process = useStore(s => s.process);
  useStore(s=>s.records); useStore(s=>s.printer);
  const none = useStore(() => listProfiles('process').length === 0), starters = usePrintMasters(none).data ?? [];
  const groups = (): PickerGroup[] => {
    if (none && starters.length) return [{ id: 'starters', title: '', items: starters.map((m) => ({ id: `starter:${m.id}`, label: m.name, sub: m.description, search: m.name })) } as PickerGroup];
    let entries = listProfiles('process');
    const hidden = currentEntry('process', process, entries);
    if (hidden) entries = [...entries, hidden];
    const ordered = [...entries].sort((a, b) => a.name.localeCompare(b.name));
    const items: PickerItem[] = ordered.map((p) => ({ id: p.key, label: p.name, meta: p === hidden ? UNAVAILABLE : undefined, search: `${p.name} ${p.vendor}`, disabled: p.reason }));
    return [{ id: 'profiles', title: '', items } as PickerGroup];
  };
  const pick = (id: string) => {
    const starter = id.startsWith('starter:') ? starters.find((m) => `starter:${m.id}` === id) : undefined;
    if (starter) void addPrintMaster(starter).then((key) => requestPreset('process', 0, key));
    else requestPreset('process', 0, id);
  };
  const info = useStore((s) => profileInfo(s, 'process', s.process)), pending = usePickerPending(!!info.name);
  const removed = info.archived ? info.name : '';
  const [adding, setAdding] = useState(false), trigger = useRef<HTMLButtonElement>(null);
  return <AddPrintMenu anchor={trigger} open={adding} onOpenChange={setAdding} onOpen={(key) => requestPreset('process', 0, key)}
    onCreated={(key) => { requestPreset('process', 0, key); openSlideOutEditor('process', key); }}>
    <div className="process-preset-picker"><Picker label="Print settings" value={process} sections groups={sameStateGroups('process', groups)} memory="orca.parity.process" onPick={pick} triggerProps={{ ref: trigger }} footer={[{ label: 'Add', run: () => requestAnimationFrame(() => setAdding(true)) }, presets('process', process)]}>{removed ? <><span className="pk-name pk-empty">Choose print settings</span><span className="pk-sub">{removed} was removed</span></> : pending ? <PickerSkeleton/> : <span className="pk-name">{info.name || 'Choose print settings'}</span>}</Picker></div>
  </AddPrintMenu>;
}
