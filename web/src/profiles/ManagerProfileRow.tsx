import { memo, useState } from 'react';
import { Check } from '../components/Check';
import { DynamicBadge } from '../components/DynamicBadge';
import { ProblemLine } from './RecordProblem';
import type { SelectionEvent } from './selection';

export interface ManagerProfileRowItem {
  key: string;
  name: string;
  note: string;
  meta?: string;
  fullName?: string;
  label?: string;
  child?: boolean;
  slot?: number;
  cover?: string | null;
  lead?: string;
  dynamic?: boolean;
  problem?: string;
  method?: string;
  loaded?: string;
}

const METHOD: Record<string, string> = {
  Flat: 'M3 6h10M3 9h10M3 12h10',
  'Curved top': 'M3 12h10M3 9.5C4.5 5.5 11.5 5.5 13 9.5',
  'Custom surface': 'M3 12h10M3 9.5c1.5-3.5 3.5-3.5 5 -1s3 2 5-2.5',
  Waves: 'M3 8c1.2-1.6 2.8-1.6 4 0s2.8 1.6 4 0 1.5-1.2 2-1M3 12h10',
  Automatic: 'M8 3v2.5M8 10.5V13M3 8h2.5M10.5 8H13M4.6 4.6l1.8 1.8M9.6 9.6l1.8 1.8M11.4 4.6 9.6 6.4M6.4 9.6l-1.8 1.8',
};

function splitName(name: string): [string, string] {
  for (const re of [/\s\S+\s\S+$/, /\s\S+$/]) { const m = re.exec(name); if (m && m[0].length <= 15 && m.index >= 4) return [name.slice(0, m.index), '\u00a0' + m[0].slice(1)]; }
  return ['', name];
}

export const ManagerProfileRow = memo(function ManagerProfileRow({ item, listId, selected, open = false, focused, selectable = true, dispatch }: {
  item: ManagerProfileRowItem;
  listId: string;
  selected: boolean;
  open?: boolean;
  focused: boolean;
  selectable?: boolean;
  dispatch(event: SelectionEvent): void;
}) {
  const [failedCover,setFailedCover]=useState<string>();
  const [head,tail]=splitName(item.name);
  const label=<span role="gridcell" className="pf-row-label"><span className="pf-item-name" {...(item.lead !== undefined && item.fullName && item.fullName !== item.name ? {'data-truncated': 'true', 'data-full-text': item.fullName} : {})}>{head && <span className="pf-name-head">{head}</span>}<span className="pf-name-tail">{tail}</span></span>{item.dynamic && <DynamicBadge/>}
      {item.meta && <span className="pf-item-meta">{item.meta}</span>}{item.note && <span className="pf-item-note">{item.note}</span>}</span>;
  return <div id={`${listId}:${item.key}`} data-profile-key={item.key} role="row" aria-label={item.label ?? item.fullName} aria-selected={selected} data-child={item.child || undefined}
    data-invalid={item.problem ? true : undefined} className={`pf-item pf-selectable${open ? ' is-on' : selected ? ' is-checked' : ''}`} tabIndex={focused ? 0 : -1}
    onMouseDown={event => { if (event.shiftKey) event.preventDefault(); }}
    onClick={event => dispatch({ type: 'CLICK', key: item.key, toggle: selectable && (event.ctrlKey || event.metaKey), shift: selectable && event.shiftKey })}>
    {selectable && <span role="gridcell" className="pf-row-check"><Check aria-label={`Select ${item.fullName ?? item.name}`} checked={selected}
      onClick={event => event.stopPropagation()} onCheckedChange={() => dispatch({ type: 'CLICK', key: item.key, toggle: true })}/></span>}
    
    {item.cover !== undefined && <span role="gridcell" className="pf-row-cover">{item.cover && failedCover !== item.cover ? <img src={item.cover} alt="" width={52} height={52} loading="lazy" decoding="async" onError={()=>setFailedCover(item.cover ?? undefined)}/> : null}</span>}
    {item.lead !== undefined && <span role="gridcell" className="pf-row-lead">{item.lead}</span>}
    {item.problem ? <span className="pf-row-stack">{label}<ProblemLine role="gridcell" className="pf-row-sub" text={item.problem}/></span>
      : item.loaded ? <span className="pf-row-stack">{label}<span role="gridcell" className="pf-row-sub">{item.loaded}</span></span> : label}
    {item.method && <span role="gridcell" className="pf-row-method" aria-label={item.method} data-truncated="true" data-full-text={item.method}><svg viewBox="0 0 16 16" width="16" height="16" aria-hidden><path d={METHOD[item.method] ?? METHOD.Flat}/></svg></span>}
  </div>;
});
