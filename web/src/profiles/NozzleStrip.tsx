import { useEffect, useState } from 'react';
import { Choice } from '../components/kit';
import { toast, useStore } from '../state/store';
import { resolveProfile } from './model';
import { nozzleModel, switchNozzle, type NozzleModel } from './nozzle-switch';
import './layer-height.css';

export function NozzleStrip({ printerKey, disabled }: { printerKey: string; disabled?: boolean }) {
  const base = useStore((s) => resolveProfile(s, 'machine', printerKey).base?.name ?? '');
  const own = useStore((s) => Number([resolveProfile(s, 'machine', printerKey).config.nozzle_diameter].flat()[0]));
  const stock = useStore((s) => Number([resolveProfile(s, 'machine', printerKey).base?.config.nozzle_diameter].flat()[0]));
  const [found, setFound] = useState<NozzleModel>();
  const [busy, setBusy] = useState(false);
  useEffect(() => {
    let live = true;
    nozzleModel(base).then((f) => live && setFound(f), (e) => live && (setFound(undefined), toast(e instanceof Error ? e.message : String(e))));
    return () => { live = false; };
  }, [base]);
  if (!found) return null;
  const custom = own > 0 && Math.abs(own - stock) > 1e-6 ? String(own) : undefined;
  const nozzles = custom && !found.model.nozzles.includes(custom) ? [...found.model.nozzles, custom].sort((a, b) => parseFloat(a) - parseFloat(b) || a.localeCompare(b)) : found.model.nozzles;
  if (nozzles.length < 2) return null;
  const choose = (nozzle: string) => {
    if (nozzle === custom) return;
    setBusy(true);
    switchNozzle(printerKey, nozzle).catch((e) => toast(e instanceof Error ? e.message : String(e), null, null, 'warning')).finally(() => setBusy(false));
  };
  return <div className="nozzle-strip"><Choice label="Nozzle" value={custom ?? found.nozzle} options={nozzles.map((n) => ({ id: n, label: n }))} disabled={disabled || busy} onChange={choose}/></div>;
}
