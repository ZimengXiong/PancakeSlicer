import type { Tray } from '../printers/connections';

export interface SendFilament { key?: string; type: string; color: string; extruder?: number; name?: string; vendor?: string; id?: string; requiredHRC?: number; printable?: number; compatibility?: number;
  flow?: string; diameter?: number; hasFilamentSwitch?: boolean; object?: boolean; support?: boolean }
export const isExternalTray = (id: number) => id === 254 || id === 255;
export function trayCode(t: Tray, trays: readonly Tray[]): string {
  if (isExternalTray(t.id)) {
    const sides = trays.filter((x) => isExternalTray(x.id)).length > 1;
    return !sides ? 'External spool' : t.extruder === 0 || (t.extruder === undefined && t.id === 254) ? 'Left external spool' : 'Right external spool';
  }
  if (t.kind === 'ams-ht') return `AMS HT ${t.ams - 127}`;
  const units = new Set(trays.filter((x) => !isExternalTray(x.id) && x.kind !== 'ams-ht').map((x) => x.ams)).size;
  return units > 1 ? `AMS ${t.ams + 1} slot ${t.slot + 1}` : `Slot ${t.slot + 1}`;
}
export const trayFilament = (t: Tray) => (!isExternalTray(t.id) && t.name) || t.type || 'Empty';
