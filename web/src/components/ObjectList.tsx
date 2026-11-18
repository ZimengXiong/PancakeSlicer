import { SlotPicker } from './SlotChip';
import { Caret } from './Caret';
import { Fragment, memo, useCallback, useLayoutEffect, useMemo, useRef, useState } from 'react';
import { Glyph } from './Glyph';
import { requestAddSettings } from './add-settings-request';
import { objectLabels } from '../plater/object-names';
import { useTab } from '../options';
import { effectiveConfig, getState, isSelected, selectedObjectIds, selectionOf, setState, toggleSelection, updateInstance, updateVolume, togglePrintable, useStore } from '../state/store';
import type { Config } from '../api/types';
import type { PlateObject, Selected } from '../state/store';
import type { PlateResult } from '../recommend';
import { volumeIcon } from '../plater/volumes';
import { renameObject, setFilament, startRename } from '../edit';
import { undoable } from '../state/history';
import { instanceBox } from '../plater/geometry';
import { Icon } from './Icon';
import { STYLE_NAMES, styleOf } from '../style-names';
import { plateResults } from '../recommend';
import { canAddPlate, OBJECT_DRAG } from '../plater/plates';
import { rangeLabel } from '../plater/ranges';
import { overrideCount } from '../plater/overrides';
import { setHoverLink, useHoverLink } from '../plater/hoverLink';
import { Sep } from '../separator';
import { paintedSlots } from '../printers/slot-check';

interface SliceRecord {
  objects?: { id: number; name: string; mode?: string; instances?: { id: number; shift_mm?: [number, number] }[] }[];
}

const linkProps = (object: number, instance?: number) => ({
  onMouseEnter: () => setHoverLink({ object, instance, source: 'list' }),
  onMouseLeave: () => setHoverLink(null),
  onFocus: () => setHoverLink({ object, instance, source: 'list' }),
  onBlur: () => setHoverLink(null),
});
const openMenu = (e: React.MouseEvent, object: number, instance?: number, volume?: number) => {
  e.preventDefault();
  const o = getState().objects.find((ob) => ob.id === object)!;
  if (volume !== undefined) {
    setState({ selected: { object, instance: o.instances[0].id, allInstances: true, volume }, contextMenu: { x: e.clientX, y: e.clientY, object, volume } });
    return;
  }
  const sel = getState().selected;
  const inSelection = instance === undefined ? (sel?.all ? o.instances.some((i) => isSelected(sel, object, i.id)) : sel?.object === object && !!sel.allInstances) : isSelected(sel, object, instance);
  setState({
    ...(inSelection ? {} : { selected: instance === undefined ? { object, instance: o.instances[0].id, allInstances: true } : { object, instance } }),
    contextMenu: { x: e.clientX, y: e.clientY, object, instance },
  });
};
const printableButton = (object: number, printable: boolean, instance?: number) => printable ? null : (
  <span role="gridcell" className="ol-cell">
  <button className="ol-printable" aria-label="Printable" aria-pressed={printable}
    onClick={(e) => { e.stopPropagation(); togglePrintable(object, instance); }}>
    <Icon name={printable ? 'check_on' : 'check_off_focused'} size={16} />
  </button>
  </span>
);
const rowKeys = (pick: () => void, object: number, instance?: number) => ({
  tabIndex: 0,
  onKeyDown: (e: React.KeyboardEvent<HTMLDivElement>) => {
    if (e.target !== e.currentTarget) return;
    if (e.key === 'Enter' || e.key === ' ') pick();
    else if (e.key === 'ContextMenu' || (e.key === 'F10' && e.shiftKey)) {
      const r = e.currentTarget.getBoundingClientRect();
      openMenu({ preventDefault() {}, clientX: r.left + 24, clientY: r.bottom } as React.MouseEvent, object, instance);
    } else return;
    e.preventDefault();
  },
});
const clickObject = (e: React.MouseEvent, object: number) => {
  const s = getState(), o = s.objects.find((ob) => ob.id === object)!;
  if (e.ctrlKey || e.metaKey) return toggleSelection(object, o.instances.map((i) => i.id));
  const from = s.selected ? s.objects.findIndex((ob) => ob.id === s.selected!.object) : -1, to = s.objects.indexOf(o);
  if (e.shiftKey && from >= 0) {
    const rows = s.objects.slice(Math.min(from, to), Math.max(from, to) + 1);
    return setState({ selected: selectionOf(s, rows.flatMap((ob) => ob.instances.map((i) => `${ob.id}:${i.id}`))) });
  }
  setState({ selected: { object, instance: o.instances[0].id, allInstances: true } });
};
const settingsCell = (n: number, name: string, pick: () => void) => (
  <span role="gridcell" className="ol-set">
    <button type="button" className={`ol-set-btn${n ? ' has' : ''}`} aria-label={n ? `${n} change${n === 1 ? '' : 's'} to ${name}` : `Add settings to ${name}`}
      onClick={(e) => { e.stopPropagation(); pick(); requestAddSettings(); }}>
      {n ? n : <Glyph name="plus" size={14} />}
    </button>
  </span>
);

const ObjectRows = memo(function ObjectRows({ o, label, selected, linkedInstance, expanded, onToggle, renaming, result, meshError, meshPlates, global, plateStyle, rowIndex, onFocus }: {
  onFocus(object: number | null): void; rowIndex: number; o: PlateObject; label: string; selected: Selected | null; linkedInstance: number | null | undefined; expanded: boolean;
  onToggle(object: number, expanded: boolean): void; renaming: number | null; result?: PlateResult;
  meshError: number; meshPlates: boolean; global: Config; plateStyle: ReturnType<typeof styleOf>;
}) {
  const linked = (instance?: number) => linkedInstance !== undefined && (instance === undefined || linkedInstance === null || linkedInstance === instance) ? ' linked' : '';
  const objSelected = selected?.volume === undefined && o.instances.some((i) => isSelected(selected, o.id, i.id));
  const multi = o.instances.length > 1;
  const instSelected = (id: number) => (selected?.all ? isSelected(selected, o.id, id) : selected?.instance === id);
  const sinking = !o.project?.placeholder && o.instances.some((i) => { const b = instanceBox(o, i); return b.min.z < -0.001 && b.max.z >= -0.001; });
  const expandable = multi || !!o.volumes?.length || !!o.ranges?.length;
  const nSettings = expanded ? Object.keys(o.settings).length : overrideCount(o);
  const pickSettings = () => setState((st) => ({ 
    selected: objSelected && st.selected?.all ? st.selected : { object: o.id, instance: o.instances[0].id, allInstances: true } }));
  const style = o.project || !Object.keys(o.settings).length ? plateStyle : styleOf({ ...global, ...o.settings });
  return (
    <div onFocusCapture={() => onFocus(o.id)} onBlurCapture={e => {if (!e.currentTarget.contains(e.relatedTarget as Node | null)) onFocus(null);}}>
      <div
        role="row" aria-rowindex={rowIndex}
        className={`ol-row ${objSelected ? 'selected' : ''}${linked()}`}
        aria-selected={objSelected}
        {...linkProps(o.id)}
        {...rowKeys(() => setState({ selected: { object: o.id, instance: o.instances[0].id, allInstances: true } }), o.id)}
        onContextMenu={(e) => openMenu(e, o.id)}
        draggable={meshPlates}
        onDragStart={(e) => e.dataTransfer.setData(OBJECT_DRAG, String(getState().selected?.all ? selectedObjectIds(getState()) : o.id))}
        onClick={(e) => clickObject(e, o.id)}
        onDoubleClick={() => startRename(o.id)}
      >
        <span role="gridcell" className="ol-name">
          {expandable ? (
            <button
              className="ol-expand" aria-expanded={expanded} aria-label={expanded ? 'Fold' : 'Open'}
              onClick={(e) => {
                e.stopPropagation();
                onToggle(o.id, !expanded);
              }}
            >
              <Caret dir={expanded ? 'down' : 'right'}/>
            </button>
          ) : (
            <span className="ol-expand" />
          )}
          <Icon name="obj_printable" size={14} />
          {renaming === o.id ? (
            <input className="ol-rename" aria-label="Object name" defaultValue={o.name} autoFocus
              onFocus={(e) => e.target.select()}
              onClick={(e) => e.stopPropagation()}
              onBlur={(e) => renameObject(o.id, e.target.value)}
              onKeyDown={(e) => {
                if (e.key === 'Enter') (e.target as HTMLInputElement).blur();
                else if (e.key === 'Escape') setState({ renaming: null });
              }} />
          ) : (
            <span className="ol-text" title={label}>
              {label}
            </span>
          )}
          {style !== plateStyle && (
            <span className="ol-badge">
              {STYLE_NAMES[style]}
            </span>
          )}
          {(result?.pct ?? 0) > 0 && <span className="ol-meta" title="Top printed curved">{result!.pct}% curved</span>}
          {o.project?.placeholder && (
            <span className="ol-badge">
              {o.project.missing ? 'no mesh' : 'loading'}
            </span>
          )}
        </span>
        {printableButton(o.id, o.instances.some((i) => i.printable !== false))}
        {o.project ? <span role="gridcell" className="ol-set" /> : settingsCell(nSettings, label, pickSettings)}
        <span role="gridcell" className="ol-fila"><SlotPicker label="Filament" value={o.extruder || 1} painted={paintedSlots(o)} disabled={false}
          onChange={(next) => setFilament(o.id, next)} /></span>
        <span role="gridcell" className="ol-status">{sinking && <button className="ol-sinking" aria-label="Drop to bed"
          onClick={(e) => { e.stopPropagation(); undoable('Drop to bed', () => o.instances.forEach((i) => updateInstance(o.id, i.id, { z: 0 }))); }} disabled={!!o.project?.placeholder}>
          <Icon name="objlist_sinking" size={16} />
        </button>}
        {meshError > 0 && <button className="ol-sinking" disabled={!!o.project || !!o.volumes?.length}
          aria-label="Repair model"
          onClick={(e) => { e.stopPropagation(); void import('../plater/meshOps').then((m) => m.fixModel(o.id)); }}>
          <Icon name="obj_warning" size={16} />
        </button>}</span>
      </div>
      {expanded && o.volumes?.map((v) => {
        const pick = () => setState({ selected: { object: o.id, instance: o.instances[0].id, allInstances: true, volume: v.id },  });
        const nv = Object.keys(v.settings).length;
        const volSelected = selected?.object === o.id && selected.volume === v.id;
        return (
          <div key={`v${v.id}`}>
            <div role="row"
              className={`ol-row ol-child ${volSelected ? 'selected' : ''}`} aria-selected={volSelected}
              {...rowKeys(() => pick(), o.id)}
              onContextMenu={(e) => openMenu(e, o.id, undefined, v.id)}
              onClick={() => pick()}
              onDoubleClick={() => setState({ renaming: v.id })}>
              <span role="gridcell" className="ol-name">
                <Icon name={volumeIcon(v.type, v.seamMode)} size={14} />
                {renaming === v.id ? (
                  <input className="ol-rename" aria-label="Part name" defaultValue={v.name} autoFocus
                    onFocus={(e) => e.target.select()}
                    onClick={(e) => e.stopPropagation()}
                    onBlur={(e) => { setState({ renaming: null }); if (e.target.value.trim() && e.target.value !== v.name) updateVolume(o.id, v.id, { name: e.target.value.trim() }, 'Rename Part'); }}
                    onKeyDown={(e) => {
                      if (e.key === 'Enter') (e.target as HTMLInputElement).blur();
                      else if (e.key === 'Escape') setState({ renaming: null });
                    }} />
                ) : <span className="ol-text" title={v.name}>{v.name}</span>}
              </span>
              {v.type === 'part' || v.type === 'modifier' ? settingsCell(nv, v.name, () => pick()) : <span role="gridcell" className="ol-set" />}
              {v.type === 'part' ? (
                <span role="gridcell" className="ol-fila"><SlotPicker label="Filament" value={v.extruder} inherit={o.extruder || 1}
                  onChange={(next) => updateVolume(o.id, v.id, { extruder: next })} /></span>
              ) : <span role="gridcell" className="ol-fila" />}
              <span role="gridcell" className="ol-status" />
            </div>
          </div>
        );
      })}
      {expanded && !!o.ranges?.length && (
        <>
          <div role="row" className="ol-row ol-child ol-group"><span role="gridcell" className="ol-name"><Icon name="height_range_layer" size={14} /><span className="ol-text">Layers</span></span></div>
          {o.ranges.map((r) => {
            const on = selected?.object === o.id && selected.range === r.id;
            const pickRange = () => setState({ selected: { object: o.id, instance: o.instances[0].id, allInstances: true, range: r.id } });
            return (
              <div key={`r${r.id}`} role="row" className={`ol-row ol-child2 ${on ? 'selected' : ''}`} aria-selected={on} {...rowKeys(pickRange, o.id)} onClick={pickRange}
               >
                <span role="gridcell" className="ol-name"><Icon name="height_range_layer" size={14} />
                <span className="ol-text">{rangeLabel(r)}{Object.keys(r.settings).length > 0 && <><Sep/>{Object.keys(r.settings).length} settings</>}</span></span>
              </div>
            );
          })}
        </>
      )}
      {multi && expanded && (
        <>
          <div role="row" className="ol-row ol-child ol-group"><span role="gridcell">Instances</span></div>
          {o.instances.map((inst, k) => (
            <div
              key={inst.id}
              role="row"
              className={`ol-row ol-child2 ${instSelected(inst.id) ? 'selected' : ''}${linked(inst.id)}`}
              aria-selected={instSelected(inst.id)}
              {...linkProps(o.id, inst.id)}
              {...rowKeys(() => setState({ selected: { object: o.id, instance: inst.id } }), o.id, inst.id)}
              onContextMenu={(e) => openMenu(e, o.id, inst.id)}
              onClick={(e) => (e.ctrlKey || e.metaKey ? toggleSelection(o.id, [inst.id]) : setState({ selected: { object: o.id, instance: inst.id } }))}
            >
              <span role="gridcell" className="ol-name">Instance {k + 1}</span>
              {printableButton(o.id, inst.printable !== false, inst.id)}
              <span role="gridcell" className="ol-set" /><span role="gridcell" className="ol-fila" /><span role="gridcell" className="ol-status" />
            </div>
          ))}
        </>
      )}
    </div>
  );
});

export const ObjectList = memo(function ObjectList() {
  useTab('process');
  const objects = useStore((s) => s.objects);
  const labels = useMemo(() => objectLabels(objects), [objects]);
  const results = useStore(plateResults);
  const selected = useStore((s) => s.selected);
  const project = useStore((s) => s.project);
  const record = useStore((s) => (s.job.state === 'done' && !s.job.stale ? (s.job.status?.record as SliceRecord | undefined) : undefined));
  const renaming = useStore((s) => s.renaming);
  const meshErrors = useStore((s) => s.meshErrors);
  const meshPlates = useStore((s) => canAddPlate(s) && s.projectPlates.length > 1);
  const link = useHoverLink('canvas');
  const [query, setQuery] = useState('');
  const [open, setOpen] = useState<Record<number, boolean>>({});
  const [focused, setFocused] = useState<number | null>(null);
  const toggleOpen = useCallback((object: number, expanded: boolean) => setOpen(prior => ({...prior, [object]: expanded})), []);
  const shown = useMemo(() => objects.filter((o) => o.name.toLowerCase().includes(query.toLowerCase())), [objects, query]);
  const list = useRef<HTMLDivElement>(null), grid = useRef<HTMLDivElement>(null);
  const [viewport, setViewport] = useState(() => ({top: 0, height: 400, row: matchMedia('(pointer: coarse)').matches ? 36 : 28}));
  const refreshViewport = useCallback(() => {
    const box = list.current?.closest<HTMLElement>('.op-body'), body = grid.current;
    if (!box || !body) return;
    const row = body.querySelector<HTMLElement>('.ol-row')?.getBoundingClientRect().height;
    const next = {top: Math.max(0, box.getBoundingClientRect().top - body.getBoundingClientRect().top), height: box.clientHeight};
    setViewport(prior => prior.top === next.top && prior.height === next.height && (!row || prior.row === row) ? prior : {...next, row: row || prior.row});
  }, []);
  useLayoutEffect(() => {
    const box = list.current?.closest<HTMLElement>('.op-body');
    if (!box) return;
    let frame = 0;
    const schedule = () => { if (!frame) frame = requestAnimationFrame(() => {frame = 0; refreshViewport();}); };
    const resize = new ResizeObserver(schedule);
    resize.observe(box);
    box.addEventListener('scroll', schedule, {passive: true});
    const pointer = matchMedia('(pointer: coarse)');
    pointer.addEventListener('change', schedule);
    refreshViewport();
    return () => {resize.disconnect(); box.removeEventListener('scroll', schedule); pointer.removeEventListener('change', schedule); cancelAnimationFrame(frame);};
  }, [refreshViewport]);
  const rows = useMemo(() => {
    let count = 0;
    return shown.map(o => {
      const expanded = open[o.id] ?? (!!o.volumes?.length || !!o.ranges?.length);
      const length = 1 + (expanded ? (o.volumes?.length ?? 0) + (o.ranges?.length ? o.ranges.length + 1 : 0) + (o.instances.length > 1 ? o.instances.length + 1 : 0) : 0);
      const row = {o, expanded, index: count + 1, top: count * viewport.row, height: length * viewport.row};
      count += length;
      return row;
    });
  }, [shown, open, viewport.row]);
  const totalHeight = rows.length ? rows[rows.length - 1].top + rows[rows.length - 1].height : 0;
  const visible = rows.filter(r => r.o.id === focused || r.o.id === renaming || r.o.volumes?.some(v => v.id === renaming) ||
    r.top + r.height >= viewport.top - viewport.row * 4 && r.top <= viewport.top + viewport.height + viewport.row * 4);
  const bottom = visible.length ? visible[visible.length - 1] : undefined;
  const revealed = useRef<Selected | null | undefined>(undefined);
  useLayoutEffect(() => {
    if (revealed.current === selected) return;
    const box = list.current?.closest<HTMLElement>('.op-body'), body = grid.current;
    if (!box || !body) return;
    const row = body.querySelector<HTMLElement>('.ol-row.selected');
    if (row) {
      const b = box.getBoundingClientRect(), r = row.getBoundingClientRect();
      if (r.top < b.top) box.scrollTop -= b.top - r.top;
      else if (r.bottom > b.bottom) box.scrollTop += r.bottom - b.bottom;
      revealed.current = selected;
    } else {
      const target = selected && rows.find(r => r.o.id === selected.object);
      if (target) box.scrollTop += target.top - (box.getBoundingClientRect().top - body.getBoundingClientRect().top);
      else revealed.current = selected;
    }
    refreshViewport();
  }, [selected, rows, viewport.top, refreshViewport]);
  const global = useStore(st => effectiveConfig(st, 'process'));
  const plateStyle = styleOf(global);

  return (
    <div className="object-list" ref={list}>
      {objects.length > 8 && (
        <div className="search-bar">
          <Icon name="search" size={16} />
          <input placeholder="Search" value={query} onChange={(e) => {
            setQuery(e.target.value);
            const box = list.current?.closest<HTMLElement>('.op-body');
            if (box) box.scrollTop = 0;
            setViewport(prior => ({...prior, top: 0}));
          }} />
        </div>
      )}
      <div className="ol-body" ref={grid} role="grid" aria-label="Objects" aria-multiselectable="true" aria-rowcount={totalHeight / viewport.row + (project && record?.objects ? record.objects.length + 1 : 0)}>
        {visible.map(({o, expanded, index, top}, at) => <Fragment key={o.id}>
          <div aria-hidden="true" style={{height: top - (at ? visible[at - 1].top + visible[at - 1].height : 0), flexShrink: 0}}/>
          <ObjectRows o={o} label={labels.get(o.id) ?? o.name} rowIndex={index} onFocus={setFocused}
          selected={selected && (selected.object === o.id || o.instances.some(i => isSelected(selected, o.id, i.id))) ? selected : null}
          linkedInstance={link?.object === o.id ? link.instance ?? null : undefined}
          expanded={expanded} onToggle={toggleOpen}
          renaming={renaming === o.id || o.volumes?.some(v => v.id === renaming) ? renaming : null}
          result={results.length > 1 ? results.find(r => r.objectId === o.id) : undefined}
          meshError={meshErrors[o.id] ?? 0} meshPlates={meshPlates} global={global} plateStyle={plateStyle}/></Fragment>)}
        <div aria-hidden="true" style={{height: totalHeight - (bottom ? bottom.top + bottom.height : 0), flexShrink: 0}}/>
        {project && record?.objects && (
          <div className="ol-record">
            <div role="row" className="ol-row ol-group"><span role="gridcell">Sliced from {project.name}</span></div>
            {record.objects.map((r) => (
              <div key={r.id} role="row" className="ol-row ol-child">
                <span role="gridcell" className="ol-name"><Icon name="obj_printable" size={14} />
                <span className="ol-text">{r.name}</span></span>
                <span role="gridcell" className="ol-meta">
                  {r.instances?.length ?? 1}× {r.mode ?? ''}
                </span>
              </div>
            ))}
          </div>
        )}
      </div>
    </div>
  );
});
