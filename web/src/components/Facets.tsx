import { useId, useState } from 'react';
import { Check } from './Check';
export interface FacetOption { value: string; label: string; count?: number }
const SHOWN = 6;
export function FacetGroup({ label, options, value, onChange, toggle: single = false }: { label: string; options: FacetOption[]; value: string[]; onChange(value: string[]): void; toggle?: boolean }) {
  const [expanded, setExpanded] = useState(false), [query, setQuery] = useState('');
  const id = useId();
  if (options.length < 2 && !value.length && !single) return null;
  const toggle = (v: string, on: boolean) => onChange(on ? [...value, v] : value.filter((x) => x !== v));
  const matching = query ? options.filter((o) => o.label.toLocaleLowerCase().includes(query.toLocaleLowerCase())) : options;
  const shown = expanded || query ? matching : matching.slice(0, SHOWN);
  return <div className="facet-group" role="group" aria-labelledby={id}>
    <div className="facet-head" id={id}>{label}</div>
    <>
      {options.length > 12 && <input className="text" aria-label={`Find ${label.toLowerCase()}`} placeholder="Find" value={query} onChange={(e) => setQuery(e.target.value)} />}
      {shown.map((o) => <label key={o.value} className="facet-row"><Check checked={value.includes(o.value)} onCheckedChange={(on) => toggle(o.value, on === true)} /><span data-full-text={o.label}>{o.label}</span>{o.count !== undefined && <span className="facet-count">{o.count}</span>}</label>)}
      {!shown.length && <span className="facet-empty">No matches</span>}
      {!query && options.length > SHOWN && <button type="button" className="more-toggle" aria-expanded={expanded} onClick={() => setExpanded(!expanded)}>{expanded ? 'Show less' : `Show ${options.length - SHOWN} more`}</button>}
    </>
  </div>;
}
export function FacetsHead({ active, onClear }: { active: number; onClear(): void }) {
  return <div className="facets-head"><span>{active ? `${active} filter${active > 1 ? 's' : ''}` : 'Filters'}</span>{active > 0 && <button type="button" className="more-toggle" onClick={onClear}>Clear</button>}</div>;
}
