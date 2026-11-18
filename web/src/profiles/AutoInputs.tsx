import { Choice } from '../components/kit';
import { Dropdown } from '../components/Dropdown';
import { setLayerHeight, useStore } from '../state/store';
import { availableHeights, effectiveLayerHeight } from './context';
import { recordOf } from './view';
import './layer-height.css';

const label = (h: number) => h.toFixed(2);

export function AutoInputs() {
  const offered = useStore((s) => availableHeights(s).join(','));
  const pending = useStore((s) => !s.resolved.answer && s.resolved.state !== 'failed' && !!recordOf(s, 'process', s.process) && !recordOf(s, 'process', s.process)?.archived);
  const height = useStore((s) => effectiveLayerHeight(s));
  const heights = offered ? offered.split(',').map(Number) : [];
  const custom = height !== null && !heights.some((h) => Math.abs(h - height) < 0.0005);
  const value = height === null ? '' : custom ? 'custom' : label(height);
  const choose = (id: string) => { if (id !== 'custom') setLayerHeight(Number(id)); };
  const extra = custom ? [{ id: 'custom', label: label(height!) }] : [];
  return <>
    {pending && <div className="rail-row rail-row-wide layer-heights" aria-hidden="true"><span className="seg" style={{ width: '100%' }}><span className="skel-block" style={{ height: 'var(--seg-h)' }}/></span></div>}
    {heights.length > 0 && <div className="rail-row rail-row-wide layer-heights">
      {heights.length > 6
        ? <Dropdown className="combo" aria-label="Layer height" value={value} onValueChange={choose} options={[...heights.map((h) => ({ value: label(h), label: `${label(h)} mm` })), ...extra.map((o) => ({ value: o.id, label: `${o.label} mm` }))]}/>
        : <Choice label="Layer height" value={value} options={[...heights.map((h) => ({ id: label(h), label: label(h) })), ...extra]} onChange={choose}/>}
    </div>}
  </>;
}
