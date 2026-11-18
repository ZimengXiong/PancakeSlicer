import { useEffect, useState } from 'react';
import { useStore } from '../state/store';
import { selectionValues } from '../profiles/view';
import { headShortName, loadHead, loadHeadIndex, slopeLimit, type Head } from '../heads';
import { SEP } from '../separator';

const num = (v: unknown) => Number(Array.isArray(v) ? v[0] : v) || 0;

export default function NonplanarSummary({ onExplain }: { onExplain?: () => void }) {
  const heads = useStore((s) => s.heads);
  const print = useStore((s) => selectionValues(s, 'process')), printer = useStore((s) => selectionValues(s, 'machine'));
  const process = print.values, machine = printer.values;
  const mode = String(process.nonplanar_mode ?? 'off');
  const id = String(machine.nonplanar_head ?? '');
  const [head, setHead] = useState<Head | null>(null);
  useEffect(() => void loadHeadIndex().catch(() => undefined), []);
  useEffect(() => {
    let live = true;
    void loadHead(id || 'generic-conservative').then((h) => live && setHead(h));
    return () => { live = false; };
  }, [id]);

  if (print.state !== 'ready' || printer.state !== 'ready' || mode === 'off') return null;
  const info = heads?.find((h) => h.id === id);
  const name = !id ? 'generic' : info ? headShortName(info).replace(/^Bambu Lab /, '') : heads ? `${id} (missing; generic)` : id;
  const text = `Head: ${name}${info && !info.calibrated ? `${SEP}uncalibrated` : ''}`;
  const details = [!id ? 'Safe generic print head' : info ? `${info.name}, ${info.calibrated ? 'calibrated' : 'uncalibrated'}` : `Head ${id}`];
  const gap = num(process.nonplanar_shell_layer_height) || num(process.layer_height);
  const cap = num(process.nonplanar_max_slope);
  if (head?.id === (id || 'generic-conservative') && gap > 0) {
    const limit = slopeLimit(head, gap);
    details.push(`Slope limit ${(cap > 0 ? Math.min(cap, limit) : limit).toFixed(1)}° at ${gap} mm`);
  }
  if (String(machine.nonplanar_kinematics ?? 'none') !== 'none' && num(machine.nonplanar_max_tilt) > 0)
    details.push(`Tilt limit ${num(machine.nonplanar_max_tilt)}°`);
  return (
    <div className="np-summary" title={details.join('\n')}>
      <span className="np-summary-text">{text}</span>
      {onExplain && <button type="button" className="np-summary-explain" title="What does this mean?" onClick={onExplain}>?</button>}
    </div>
  );
}
