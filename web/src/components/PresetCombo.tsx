import type { PresetType } from '../api/types';
import { switchPreset } from '../actions';
import { getState, setState, toast } from '../state/store';
import { selectChanges } from '../state/changes';
import { profileInfo } from '../profiles/view';
import { saveDiscardDialog } from './Ask';
import { reviewChanges } from '../profiles/PresetBar';

export async function requestPreset(type: PresetType, slot: number, name: string) {
  if (type === 'machine') {
    const s = getState(), unsaved = selectChanges(s, 'machine', 0).filter((c) => c.scope === 'project').length, current = profileInfo(s, 'machine', s.printer).name;
    if (unsaved) {
      const choice = await saveDiscardDialog('Unsaved printer changes', `${current} has ${unsaved} unsaved change${unsaved === 1 ? '' : 's'}. Save ${unsaved === 1 ? 'it' : 'them'} to ${current} before switching printers?`, `Save to ${current}`);
      if (!choice) return;
      if (choice === 'save') try { await saveToProfile('machine', 0); } catch (e) { toast('presetcombo_failed', { detail: e instanceof Error ? e.message : String(e) }); return; }
    }
  }
  const before = getState().printer;
  let result;try{result=switchPreset(type,slot,name,'transfer');}catch(error){toast('presetcombo_failed_2', { detail: error instanceof Error?error.message:String(error) });return;}
  const after = getState();
  if (type === 'machine' && after.slideOut?.type === 'machine' && after.slideOut.key === before && after.printer !== before) setState({ slideOut: { ...after.slideOut, key: after.printer } });
  const {kept,dropped}=result;
  if (!kept && !dropped) return;
  let reviewType = type, reviewSlot = slot;
  if (type === 'machine') {
    const s = getState();
    if (!Object.keys(s.dirty.machine).length) {
      reviewType = Object.keys(s.dirty.process).length ? 'process' : 'filament';
      reviewSlot = Math.max(0, s.projectPresets?.filaments.findIndex((f) => Object.keys(f).length) ?? 0);
    }
  }
  toast('changes_keptx', { kept, value: dropped ? `${SEP}${dropped} unavailable` : '' }, { action: {
    label: 'Review', run: () => reviewChanges(reviewType, reviewSlot),
  } });
}

import { PrinterPicker, FilamentPicker } from '../profiles/Pickers';
import { ProcessPresetPicker } from './ProcessPresetPicker';
import { SEP } from '../separator';
import { saveToProfile } from '../profiles/save-to-profile';
export function PresetCombo({ type, slot = 0, className }: { type: PresetType; slot?: number; className?: string }) {
  return <span className={`preset-combo ${className ?? ''}`}>{type === 'machine' ? <PrinterPicker /> : type === 'process' ? <ProcessPresetPicker /> : <FilamentPicker slot={slot} />}</span>;
}
