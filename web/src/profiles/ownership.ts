import type { PresetType } from '../api/types';
import type { ProfileKey } from './identity';

export type { SwapMode } from './records';
export type CalibrationRow = import('./records').CalibrationRecord;
export interface ProjectProfileReferences {
  schema: 2;
  printer?: import('./records').PrinterRecord;
  process?: import('./records').ProcessRecord;
  filaments: (import('./records').FilamentRecord | undefined)[];
  slots?: import('./records').SlotRecord[];
}
export type ProfileRecoveryReason = 'missing-profile' | 'missing-base' | 'missing-owner' | 'ambiguous' | 'cycle' | 'foreign-owner' | 'archived' | 'incompatible';
export interface ProfileRecovery {
  reason: ProfileRecoveryReason;
  key: ProfileKey;
  type: PresetType;
}
